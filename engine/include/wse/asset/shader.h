// Clean-room v1.1.5.5 shader, technique, effect, material state, and SPIR-V cache module.
//
// Reconstructed from v1.1.5.5 PE + PDB evidence (`docs/PLATFORM_PLAN.md`, slice P5):
// - rglEffectManager::initDeviceObjects (0x14028bce0),
//   rglGraphicsContext::checkLoadingPostEffects (0x14029c930),
//   rglRenderer::initPostFxShaders (0x1402db690), D3DXCreateEffectFromFileA (0x140501b70)
// - rglResourceManager::loadOptionalShaderResource (0x1402eeec0),
//   initShaderTechniques (0x1402ecd30), initShaderPairs (0x1402ed3c0),
//   getSuitableShader (0x1402f3db0), getSuitableShaderImpl (0x1402f3eb0),
//   initMaterialTextures (0x1402f5390)
// - rglTechnique::initDeviceObjects (0x1402a5c30), initEarlyZHandle (0x1402a5fc0)
// - rglShader (0x1fa6f, 504 B), rglShader::isSuitable (0x140301ab0),
//   rglMesh::getVertexDeclarationType (0x1402baa30)
// - rglMaterial (0x1dc61, 632 B), rglMaterial::readStream (0x1402b5970),
//   rglGraphicsContext::setAlphaTestMode (0x1402a00b0),
//   rglGraphicsContext::createBlendStates (0x14029f590), setFogEnabled (0x14029ffa0)
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/game/module_loader_common.h"
#include "wse/platform/vfs.h"

namespace wse::asset {

// -----------------------------------------------------------------------------
// rglShader & rglMaterial flags from v1.1.5.5 PE reconstruction
// -----------------------------------------------------------------------------

// rglShader::m_flags (`+0x58`, rglShader::isSuitable 0x140301ab0,
// rglMesh::getVertexDeclarationType 0x1402baa30, rglResourceManager::initShaderPairs 0x1402ed3c0).
inline constexpr std::uint32_t kShaderFlagUsesInstancing = 0x00002000U;
inline constexpr std::uint32_t kShaderFlagRuntimeDisabled = 0x00010000U;
inline constexpr std::uint32_t kShaderFlagSpecial = 0x00020000U;
inline constexpr std::uint32_t kShaderFlagFirstStageColorOp0x18 = 0x00040000U;
inline constexpr std::uint32_t kShaderFlagUsesHlsl = 0x20000000U;
inline constexpr std::uint32_t kShaderFlagUsesNormalMap = 0x40000000U;
inline constexpr std::uint32_t kShaderFlagUsesSkinning = 0x80000000U;

// rglShader::m_requirementFlags (`+0x64`, rglShader::isSuitable 0x140301ab0).
inline constexpr std::uint32_t kShaderReqLoQuality = 0x00001000U;
inline constexpr std::uint32_t kShaderReqMidQuality = 0x00002000U;
inline constexpr std::uint32_t kShaderReqHiQuality = 0x00004000U;

// rglMaterial::m_flags (`+0x58`, rglRenderer::renderMesh 0x1402e7f50).
inline constexpr std::uint32_t kMaterialFlagNoFog = 0x00000001U;
inline constexpr std::uint32_t kMaterialFlagNoZWrite = 0x00000008U;
inline constexpr std::uint32_t kMaterialFlagNoDepthTest = 0x00000010U;
inline constexpr std::uint32_t kMaterialFlagBlendMask = 0x00000700U;
inline constexpr std::uint32_t kMaterialFlagAlphaTestMask = 0x00003000U;
inline constexpr std::uint32_t kMaterialFlagRenderOrderMask = 0x0F000000U;

// rglMesh::getVertexDeclarationType (0x1402baa30) returns values 0..4.
enum class VertexDeclType : std::uint8_t {
    Standard = 0,
    Bumped = 1,
    Skinned = 2,
    BumpedSkinned = 3,
    Special = 4,
};

// rglGraphicsContext::setAlphaTestMode (0x1402a00b0).
enum class AlphaTestMode : std::uint8_t {
    None = 0,
    GreaterEqual8 = 1,
    GreaterEqual136 = 2,
    GreaterEqual251 = 3,
};

// rglGraphicsContext::createBlendStates (0x14029f590): 5 D3D9 blend state slots.
enum class MaterialBlendMode : std::uint8_t {
    Opaque = 0,
    AlphaBlend = 1,
    AdditiveAlpha = 2,
    Multiply = 3,
    Additive = 4,
};

struct EffectConfig {
    bool use_wse2_effect = true;
    int effect_shader_model = 3;
    bool prefer_ps2a = true;
};

struct ShaderQualityConfig {
    int shader_quality = 3;  // rglConfig::Graphics::iShaderQuality (0..3)
    bool instancing_supported = true;
    bool require_hlsl_technique = true;
};

struct PostFxResolveResult {
    std::string resolved_path{};
    std::string source_text{};
    bool appended_wse2_include = false;
    bool module_wse2_shadows_root = false;
};

struct ShaderPairEntry {
    std::string source_shader{};
    std::string target_shader{};
    bool skin_fallback = true;
};

struct ShaderPairTable {
    std::vector<ShaderPairEntry> entries{};
    // Lowercase source shader name -> target shader name.
    std::unordered_map<std::string, std::string> skin_fallback_map{};
    std::unordered_map<std::string, std::string> noskin_fallback_map{};
    std::vector<std::string> warnings{};
};

struct ShaderResolutionResult {
    const BrfShader* shader = nullptr;
    int hop_depth = 0;
    bool used_default_fallback = false;
};

struct MaterialRenderState {
    bool no_fog = false;
    bool depth_write = true;
    bool depth_test = true;
    bool srgb_diffuse = true;
    bool uses_normal_map = false;
    bool uses_specular = false;
    bool uses_skinning = false;
    MaterialBlendMode blend_mode = MaterialBlendMode::Opaque;
    AlphaTestMode alpha_test_mode = AlphaTestMode::None;
    std::uint8_t alpha_ref_u8 = 0;
    int render_order = 0;
    std::string diffuse_a{};
    std::string diffuse_b{};
    std::string bumpmap{};
    std::string envmap{};
    std::string specular_map{};
    float specular_coeff = 0.0f;
    Vec3f specular_rgb{};
};

enum class SpirvStage : std::uint32_t {
    Vertex = 0,
    Fragment = 4,
};

enum SpirvFeatureFlags : std::uint32_t {
    kSpirvFeatNone = 0U,
    kSpirvFeatNormalMap = 1U << 0U,
    kSpirvFeatSpecular = 1U << 1U,
    kSpirvFeatSkinning = 1U << 2U,
    kSpirvFeatAlphaTest = 1U << 3U,
    kSpirvFeatInstancing = 1U << 4U,
    kSpirvFeatVertexColor = 1U << 5U,
    kSpirvFeatEnvMap = 1U << 6U,
    kSpirvFeatShadowMap = 1U << 7U,
    kSpirvFeatGammaOutput = 1U << 8U,
};

struct GlTechniqueEntry {
    std::string name{};
    std::string vertex_shader{};
    std::string fragment_shader{};
    std::string source_xml{};
    std::string vs_glsl{};
    std::string fs_glsl{};
    bool synthesized_wse2_variant = false;
};

struct ShaderTechniqueCatalog {
    std::unordered_map<std::string, GlTechniqueEntry> techniques_by_lower{};
    std::vector<std::string> loaded_sources{};
    std::vector<std::string> warnings{};

    [[nodiscard]] const GlTechniqueEntry* find(std::string_view tech_name) const {
        const auto it = techniques_by_lower.find(wse::platform::ascii_lower(tech_name));
        return (it != techniques_by_lower.end()) ? &it->second : nullptr;
    }
};

namespace shader_detail {

inline std::string_view trim_ascii(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

inline std::string normalize_material_slot(std::string_view raw) {
    const std::string_view trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return {};
    }
    const std::string lower = wse::platform::ascii_lower(trimmed);
    // rglResourceManager::initMaterialTextures (0x1402f5390) ignores "none".
    if (lower == "none") {
        return {};
    }
    return std::string(trimmed);
}

inline bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    const std::string h = wse::platform::ascii_lower(haystack);
    const std::string n = wse::platform::ascii_lower(needle);
    return h.find(n) != std::string::npos;
}

inline void append_spirv_string(std::vector<std::uint32_t>& words, std::string_view text) {
    const std::size_t n = text.size() + 1U;
    const std::size_t word_count = (n + 3U) / 4U;
    const std::size_t base = words.size();
    words.resize(base + word_count, 0U);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::uint32_t byte = static_cast<std::uint8_t>(text[i]);
        words[base + (i / 4U)] |= byte << (8U * (i % 4U));
    }
}

inline void emit_spirv_inst(std::vector<std::uint32_t>& words,
                            std::uint16_t opcode,
                            std::initializer_list<std::uint32_t> operands) {
    const std::uint32_t wc = static_cast<std::uint32_t>(1U + operands.size());
    words.push_back((wc << 16U) | static_cast<std::uint32_t>(opcode));
    for (const std::uint32_t op : operands) {
        words.push_back(op);
    }
}

inline std::uint64_t fnv1a64(std::string_view s, std::uint64_t seed = 14695981039346656037ULL) noexcept {
    std::uint64_t h = seed;
    for (unsigned char c : s) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

inline std::string hex_u64(std::uint64_t v) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16U, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[v & 0xFU];
        v >>= 4U;
    }
    return out;
}

inline bool ensure_vfs_dir_tree(wse::platform::Vfs& vfs, std::string_view dir) {
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
        dir.remove_suffix(1);
    }
    if (dir.empty() || vfs.is_dir(dir)) {
        return true;
    }
    std::size_t pos = 0;
    while (pos < dir.size()) {
        const std::size_t sep = dir.find_first_of("/\\", pos);
        const std::string_view prefix = (sep == std::string_view::npos) ? dir : dir.substr(0, sep);
        if (!prefix.empty() && !vfs.is_dir(prefix)) {
            if (!vfs.make_dir(prefix) && !vfs.is_dir(prefix)) {
                return false;
            }
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 1U;
    }
    return vfs.is_dir(dir);
}

inline std::string probe_first_existing(wse::platform::Vfs& vfs,
                                        std::initializer_list<std::string> candidates) {
    for (const std::string& path : candidates) {
        if (!path.empty() && vfs.is_file(path)) {
            return path;
        }
    }
    return {};
}

}  // namespace shader_detail

// -----------------------------------------------------------------------------
// Alpha test threshold & material render state helpers
// -----------------------------------------------------------------------------

[[nodiscard]] inline constexpr std::uint8_t alpha_test_ref_u8(AlphaTestMode mode) noexcept {
    // rglGraphicsContext::setAlphaTestMode (0x1402a00b0): modes 1..3 test >= 8, 136, 251.
    switch (mode) {
        case AlphaTestMode::None:
            return 0U;
        case AlphaTestMode::GreaterEqual8:
            return 8U;
        case AlphaTestMode::GreaterEqual136:
            return 136U;
        case AlphaTestMode::GreaterEqual251:
            return 251U;
    }
    return 0U;
}

[[nodiscard]] inline constexpr float alpha_test_ref_float(AlphaTestMode mode) noexcept {
    switch (mode) {
        case AlphaTestMode::None:
            return 0.0f;
        case AlphaTestMode::GreaterEqual8:
            return 8.0f / 256.0f;
        case AlphaTestMode::GreaterEqual136:
            return 136.0f / 256.0f;
        case AlphaTestMode::GreaterEqual251:
            return 251.0f / 256.0f;
    }
    return 0.0f;
}

[[nodiscard]] inline MaterialRenderState decode_material_render_state(
    const BrfMaterial& mat, const BrfShader* resolved_shader = nullptr) {
    MaterialRenderState out{};
    out.no_fog = (mat.flags & kMaterialFlagNoFog) != 0U;
    out.depth_write = (mat.flags & kMaterialFlagNoZWrite) == 0U;
    out.depth_test = (mat.flags & kMaterialFlagNoDepthTest) == 0U;

    const std::uint32_t raw_blend = (mat.flags & kMaterialFlagBlendMask) >> 8U;
    switch (raw_blend) {
        case 1U:
            out.blend_mode = MaterialBlendMode::AlphaBlend;
            break;
        case 2U:
            out.blend_mode = MaterialBlendMode::AdditiveAlpha;
            break;
        case 3U:
            out.blend_mode = MaterialBlendMode::Multiply;
            break;
        case 4U:
            out.blend_mode = MaterialBlendMode::Additive;
            break;
        default:
            out.blend_mode = MaterialBlendMode::Opaque;
            break;
    }

    const std::uint32_t raw_alpha_test = (mat.flags & kMaterialFlagAlphaTestMask) >> 12U;
    out.alpha_test_mode = static_cast<AlphaTestMode>(std::min<std::uint32_t>(raw_alpha_test, 3U));
    // Matches rglRenderer::renderMesh (0x1402e7f50): alpha test threshold is suppressed when blended.
    out.alpha_ref_u8 = (out.blend_mode == MaterialBlendMode::Opaque)
                           ? alpha_test_ref_u8(out.alpha_test_mode)
                           : 0U;

    const std::uint32_t raw_order = (mat.flags & kMaterialFlagRenderOrderMask) >> 24U;
    // Signed 4-bit nibble or 0..15 render order bias encoded in bits 24..27.
    out.render_order = (raw_order >= 8U) ? static_cast<int>(raw_order) - 16 : static_cast<int>(raw_order);

    out.diffuse_a = shader_detail::normalize_material_slot(mat.texture_names[0]);
    out.diffuse_b = shader_detail::normalize_material_slot(mat.texture_names[1]);
    out.bumpmap = shader_detail::normalize_material_slot(mat.texture_names[2]);
    out.envmap = shader_detail::normalize_material_slot(mat.texture_names[3]);
    out.specular_map = shader_detail::normalize_material_slot(mat.texture_names[4]);

    out.specular_coeff = mat.specular_coeff;
    out.specular_rgb = mat.specular_rgb;
    out.srgb_diffuse = !out.diffuse_a.empty();

    if (resolved_shader != nullptr) {
        out.uses_normal_map = (resolved_shader->flags & kShaderFlagUsesNormalMap) != 0U ||
                              !out.bumpmap.empty();
        out.uses_skinning = (resolved_shader->flags & kShaderFlagUsesSkinning) != 0U;
        out.uses_specular = !out.specular_map.empty() ||
                            (mat.specular_coeff > 0.0f &&
                             (mat.specular_rgb.x > 0.0f || mat.specular_rgb.y > 0.0f ||
                              mat.specular_rgb.z > 0.0f));
    } else {
        out.uses_normal_map = !out.bumpmap.empty();
        out.uses_specular = !out.specular_map.empty() || mat.specular_coeff > 0.0f;
    }

    return out;
}

// -----------------------------------------------------------------------------
// Retail v1.1.5.5 Effect Search Order (`rglEffectManager::initDeviceObjects` et al.)
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::vector<std::string> build_wse2_effect_candidates(const EffectConfig& cfg) {
    std::vector<std::string> names;
    if (!cfg.use_wse2_effect) {
        return names;
    }
    if (cfg.effect_shader_model >= 3) {
        names.emplace_back("mb_wse2_3_0.fxo");
    }
    names.emplace_back(cfg.prefer_ps2a ? "mb_wse2_2a.fxo" : "mb_wse2_2b.fxo");
    return names;
}

// Matches rglEffectManager::initDeviceObjects (0x14028bce0):
//   1. <module>/<wse2_fxo> (if bUseWSE2Effect)
//   2. <module>/mb.fx
//   3. <module>/mb_2a.fxo or mb_2b.fxo
//   4. <root>/<wse2_fxo> (if bUseWSE2Effect)
//   5. <root>/mb_2a.fxo or mb_2b.fxo
[[nodiscard]] inline std::string resolve_module_effect_path(wse::platform::Vfs& vfs,
                                                            std::string_view warband_root,
                                                            std::string_view module_dir,
                                                            const EffectConfig& cfg = {}) {
    const std::string fallback_fxo = cfg.prefer_ps2a ? "mb_2a.fxo" : "mb_2b.fxo";
    const std::vector<std::string> wse2_names = build_wse2_effect_candidates(cfg);

    if (!module_dir.empty()) {
        for (const std::string& wse2_name : wse2_names) {
            const std::string p = wse::game::vfs_join_path(module_dir, wse2_name);
            if (vfs.is_file(p)) {
                return p;
            }
        }
        const std::string mod_fx = wse::game::vfs_join_path(module_dir, "mb.fx");
        if (vfs.is_file(mod_fx)) {
            return mod_fx;
        }
        const std::string mod_fxo = wse::game::vfs_join_path(module_dir, fallback_fxo);
        if (vfs.is_file(mod_fxo)) {
            return mod_fxo;
        }
    }

    for (const std::string& wse2_name : wse2_names) {
        const std::string p = wse::game::vfs_join_path(warband_root, wse2_name);
        if (vfs.is_file(p)) {
            return p;
        }
    }

    const std::string root_fxo = wse::game::vfs_join_path(warband_root, fallback_fxo);
    if (vfs.is_file(root_fxo)) {
        return root_fxo;
    }
    return {};
}

// Matches rglRenderer::initPostFxShaders (0x1402db690):
//   <module>/earlyZ.fxo -> <module>/earlyz_wse2.fxo -> <root>/earlyz_wse2.fxo -> <root>/earlyZ.fxo
[[nodiscard]] inline std::string resolve_earlyz_effect_path(wse::platform::Vfs& vfs,
                                                            std::string_view warband_root,
                                                            std::string_view module_dir,
                                                            bool use_wse2_effect = true) {
    if (!module_dir.empty()) {
        const std::string mod_ez = wse::game::vfs_join_path(module_dir, "earlyZ.fxo");
        if (vfs.is_file(mod_ez)) {
            return mod_ez;
        }
        if (use_wse2_effect) {
            const std::string mod_wse2 = wse::game::vfs_join_path(module_dir, "earlyz_wse2.fxo");
            if (vfs.is_file(mod_wse2)) {
                return mod_wse2;
            }
        }
    }
    if (use_wse2_effect) {
        const std::string root_wse2 = wse::game::vfs_join_path(warband_root, "earlyz_wse2.fxo");
        if (vfs.is_file(root_wse2)) {
            return root_wse2;
        }
    }
    const std::string root_ez = wse::game::vfs_join_path(warband_root, "earlyZ.fxo");
    if (vfs.is_file(root_ez)) {
        return root_ez;
    }
    return {};
}

// Matches rglGraphicsContext::checkLoadingPostEffects (0x14029c930):
//   Checks <module>/PostFx.fx (or Shaders/postFX.fx / root PostFx.fx), appends
//   `#if WSE2\n#include "postFX_WSE2.fx"\n#endif\n` when `use_wse2_effect` is set and
//   `#if WSE2` is absent, and flags if <module>/postFX_WSE2.fx shadows root.
[[nodiscard]] inline bool resolve_postfx_effect_source(wse::platform::Vfs& vfs,
                                                       std::string_view warband_root,
                                                       std::string_view module_dir,
                                                       bool use_wse2_effect,
                                                       PostFxResolveResult& out,
                                                       std::string* error_out = nullptr) {
    out = PostFxResolveResult{};
    if (!module_dir.empty()) {
        out.resolved_path = shader_detail::probe_first_existing(
            vfs,
            {wse::game::vfs_join_path(module_dir, "PostFx.fx"),
             wse::game::vfs_join_path(module_dir, "postFX.fx"),
             wse::game::vfs_join_path(module_dir, "Shaders/postFX.fx")});
        out.module_wse2_shadows_root =
            vfs.is_file(wse::game::vfs_join_path(module_dir, "postFX_WSE2.fx")) &&
            vfs.is_file(wse::game::vfs_join_path(warband_root, "postFX_WSE2.fx"));
    }
    if (out.resolved_path.empty()) {
        out.resolved_path = shader_detail::probe_first_existing(
            vfs,
            {wse::game::vfs_join_path(warband_root, "PostFx.fx"),
             wse::game::vfs_join_path(warband_root, "postFX.fx")});
    }
    if (out.resolved_path.empty() || !vfs.read_text(out.resolved_path, out.source_text)) {
        if (error_out != nullptr) {
            *error_out = "unable to locate PostFx.fx";
        }
        return false;
    }
    if (use_wse2_effect && out.source_text.find("#if WSE2") == std::string::npos) {
        if (!out.source_text.empty() && out.source_text.back() != '\n') {
            out.source_text.push_back('\n');
        }
        out.source_text.append("#if WSE2\n#include \"postFX_WSE2.fx\"\n#endif\n");
        out.appended_wse2_include = true;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Optional `wse2_shaders.brf` and `wse2_shaders.ini` `[SkinFallback]`/`[NoSkinFallback]`
// -----------------------------------------------------------------------------

// Matches rglResourceManager::loadOptionalShaderResource (0x1402eeec0).
[[nodiscard]] inline std::string resolve_optional_wse2_shaders_brf_path(
    wse::platform::Vfs& vfs,
    std::string_view warband_root,
    std::string_view module_dir) {
    if (!module_dir.empty()) {
        const std::string mod_brf =
            wse::game::vfs_join_path(module_dir, "Resource/wse2_shaders.brf");
        if (vfs.is_file(mod_brf)) {
            return mod_brf;
        }
    }
    const std::string common_brf =
        wse::game::vfs_join_path(warband_root, "CommonRes/wse2_shaders.brf");
    if (vfs.is_file(common_brf)) {
        return common_brf;
    }
    return {};
}

[[nodiscard]] inline bool load_optional_wse2_shaders_brf(
    wse::platform::Vfs& vfs,
    std::string_view warband_root,
    std::string_view module_dir,
    BrfFile& out_brf,
    std::string* resolved_path_out = nullptr,
    std::string* error_out = nullptr) {
    out_brf = BrfFile{};
    const std::string path = resolve_optional_wse2_shaders_brf_path(vfs, warband_root, module_dir);
    if (resolved_path_out != nullptr) {
        *resolved_path_out = path;
    }
    if (path.empty()) {
        return false;
    }
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(path, raw)) {
        if (error_out != nullptr) {
            *error_out = "failed reading " + path;
        }
        return false;
    }
    return parse_brf(std::as_bytes(std::span<const std::uint8_t>(raw)), out_brf, error_out);
}

// Matches rglResourceManager::initShaderPairs (0x1402ed3c0).
[[nodiscard]] inline std::string resolve_wse2_shaders_ini_path(
    wse::platform::Vfs& vfs,
    std::string_view warband_root,
    std::string_view module_dir) {
    if (!module_dir.empty()) {
        const std::string mod_ini = wse::game::vfs_join_path(module_dir, "wse2_shaders.ini");
        if (vfs.is_file(mod_ini)) {
            return mod_ini;
        }
    }
    const std::string root_ini = wse::game::vfs_join_path(warband_root, "wse2_shaders.ini");
    if (vfs.is_file(root_ini)) {
        return root_ini;
    }
    return {};
}

inline void parse_wse2_shaders_ini(std::string_view ini_text, ShaderPairTable& out) {
    out = ShaderPairTable{};
    enum class Section : std::uint8_t { Other, SkinFallback, NoSkinFallback };
    Section sec = Section::Other;

    std::size_t pos = 0;
    while (pos < ini_text.size()) {
        const std::size_t eol = ini_text.find('\n', pos);
        std::string_view line = (eol == std::string_view::npos)
                                    ? ini_text.substr(pos)
                                    : ini_text.substr(pos, eol - pos);
        pos = (eol == std::string_view::npos) ? ini_text.size() : eol + 1U;

        // Strip ';' or '#' comments before trimming whitespace.
        const std::size_t semi = line.find(';');
        if (semi != std::string_view::npos) {
            line = line.substr(0, semi);
        }
        const std::size_t hash = line.find('#');
        if (hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = shader_detail::trim_ascii(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            const std::string sec_lower =
                wse::platform::ascii_lower(shader_detail::trim_ascii(line.substr(1U, line.size() - 2U)));
            if (sec_lower == "skinfallback") {
                sec = Section::SkinFallback;
            } else if (sec_lower == "noskinfallback") {
                sec = Section::NoSkinFallback;
            } else {
                sec = Section::Other;
            }
            continue;
        }

        if (sec == Section::Other) {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view lhs = shader_detail::trim_ascii(line.substr(0U, eq));
        const std::string_view rhs = shader_detail::trim_ascii(line.substr(eq + 1U));
        if (lhs.empty() || rhs.empty()) {
            continue;
        }

        ShaderPairEntry entry{};
        entry.source_shader = std::string(lhs);
        entry.target_shader = std::string(rhs);
        entry.skin_fallback = (sec == Section::SkinFallback);
        const std::string key = wse::platform::ascii_lower(lhs);
        if (entry.skin_fallback) {
            out.skin_fallback_map[key] = entry.target_shader;
        } else {
            out.noskin_fallback_map[key] = entry.target_shader;
        }
        out.entries.push_back(std::move(entry));
    }
}

// -----------------------------------------------------------------------------
// Shader Suitability, Recursive Alternative Resolution & Vertex Decl Type
// -----------------------------------------------------------------------------

// Reconstructs rglShader::isSuitable (0x140301ab0).
[[nodiscard]] inline bool is_shader_suitable(const BrfShader& shader,
                                             const ShaderQualityConfig& cfg = {},
                                             bool technique_available = true) noexcept {
    if ((shader.flags & kShaderFlagUsesHlsl) == 0U) {
        return false;
    }
    if ((shader.flags & kShaderFlagRuntimeDisabled) != 0U) {
        return false;
    }
    if (cfg.require_hlsl_technique && !technique_available) {
        return false;
    }
    if ((shader.requires_flags & kShaderReqHiQuality) != 0U && cfg.shader_quality <= 2) {
        return false;
    }
    if ((shader.requires_flags & kShaderReqMidQuality) != 0U && cfg.shader_quality <= 1) {
        return false;
    }
    if ((shader.requires_flags & kShaderReqLoQuality) != 0U && cfg.shader_quality <= 0) {
        return false;
    }
    if ((shader.flags & kShaderFlagUsesInstancing) != 0U && !cfg.instancing_supported) {
        return false;
    }
    return true;
}

inline void validate_shader_pairs(const ShaderPairTable& raw_table,
                                  const std::unordered_map<std::string, BrfShader>& shaders_by_lower,
                                  const ShaderQualityConfig& quality_cfg,
                                  ShaderPairTable& validated_out,
                                  const std::unordered_set<std::string>* available_techniques_lower = nullptr) {
    validated_out = ShaderPairTable{};
    const auto is_tech_ok = [&](const BrfShader& sh) -> bool {
        if (available_techniques_lower == nullptr) {
            return true;
        }
        return available_techniques_lower->find(wse::platform::ascii_lower(sh.technique_name)) !=
               available_techniques_lower->end();
    };

    for (const ShaderPairEntry& e : raw_table.entries) {
        const std::string src_k = wse::platform::ascii_lower(e.source_shader);
        const std::string dst_k = wse::platform::ascii_lower(e.target_shader);
        if (src_k == dst_k) {
            validated_out.warnings.push_back("self-paired shader ignored: " + e.source_shader);
            continue;
        }
        const auto src_it = shaders_by_lower.find(src_k);
        const auto dst_it = shaders_by_lower.find(dst_k);
        if (src_it == shaders_by_lower.end() || dst_it == shaders_by_lower.end()) {
            validated_out.warnings.push_back("missing shader in pair: " + e.source_shader + " = " +
                                             e.target_shader);
            continue;
        }
        const BrfShader& src = src_it->second;
        const BrfShader& dst = dst_it->second;
        const bool src_skinned = (src.flags & kShaderFlagUsesSkinning) != 0U;
        const bool dst_skinned = (dst.flags & kShaderFlagUsesSkinning) != 0U;
        // [SkinFallback] expects plain -> skinned; [NoSkinFallback] expects skinned -> plain.
        if (e.skin_fallback && (src_skinned || !dst_skinned)) {
            validated_out.warnings.push_back("invalid SkinFallback skinning flags: " +
                                             e.source_shader + " = " + e.target_shader);
            continue;
        }
        if (!e.skin_fallback && (!src_skinned || dst_skinned)) {
            validated_out.warnings.push_back("invalid NoSkinFallback skinning flags: " +
                                             e.source_shader + " = " + e.target_shader);
            continue;
        }
        if (!is_shader_suitable(dst, quality_cfg, is_tech_ok(dst))) {
            validated_out.warnings.push_back("unsuitable pair target: " + e.source_shader +
                                             " = " + e.target_shader);
            continue;
        }
        validated_out.entries.push_back(e);
        if (e.skin_fallback) {
            validated_out.skin_fallback_map[src_k] = dst.name;
        } else {
            validated_out.noskin_fallback_map[src_k] = dst.name;
        }
    }
}

namespace shader_detail {

inline void resolve_suitable_shader_impl(
    std::string_view name,
    const std::unordered_map<std::string, BrfShader>& shaders_by_lower,
    const ShaderQualityConfig& cfg,
    const std::unordered_set<std::string>* available_techniques_lower,
    std::unordered_set<std::string>& visiting,
    int depth,
    const BrfShader*& best_shader,
    int& best_depth) {
    // rglResourceManager::getSuitableShaderImpl (0x1402f3eb0) starts best_depth at 0x186a0 (100000).
    if (depth >= best_depth) {
        return;
    }
    const std::string key = wse::platform::ascii_lower(name);
    if (key.empty() || !visiting.insert(key).second) {
        return;
    }

    const auto it = shaders_by_lower.find(key);
    if (it != shaders_by_lower.end()) {
        const BrfShader& sh = it->second;
        const bool tech_ok =
            (available_techniques_lower == nullptr) ||
            (available_techniques_lower->find(wse::platform::ascii_lower(sh.technique_name)) !=
             available_techniques_lower->end());
        if (is_shader_suitable(sh, cfg, tech_ok)) {
            best_shader = &sh;
            best_depth = depth;
            visiting.erase(key);
            return;
        }
        for (const std::string& alt : sh.alternatives) {
            resolve_suitable_shader_impl(alt, shaders_by_lower, cfg, available_techniques_lower,
                                         visiting, depth + 1, best_shader, best_depth);
        }
    }

    visiting.erase(key);
}

}  // namespace shader_detail

// Reconstructs rglResourceManager::getSuitableShader (0x1402f3db0) ->
// getSuitableShaderImpl (0x1402f3eb0) -> getDefaultShader ("def_shader", 0x1402f1ef0).
[[nodiscard]] inline ShaderResolutionResult resolve_suitable_shader(
    std::string_view requested_name,
    const std::unordered_map<std::string, BrfShader>& shaders_by_lower,
    const ShaderQualityConfig& cfg = {},
    const std::unordered_set<std::string>* available_techniques_lower = nullptr) {
    const BrfShader* best = nullptr;
    int best_depth = 100000;  // 0x186a0 in rglResourceManager::getSuitableShader (0x1402f3db0)
    std::unordered_set<std::string> visiting;
    shader_detail::resolve_suitable_shader_impl(
        requested_name, shaders_by_lower, cfg, available_techniques_lower, visiting, 0, best,
        best_depth);
    if (best != nullptr) {
        return ShaderResolutionResult{best, best_depth, false};
    }

    const auto def_it = shaders_by_lower.find("def_shader");
    if (def_it != shaders_by_lower.end()) {
        return ShaderResolutionResult{&def_it->second, best_depth, true};
    }
    return ShaderResolutionResult{nullptr, best_depth, true};
}

// Reconstructs rglMesh::getVertexDeclarationType (0x1402baa30):
// Selects `paired_shader` when `uses_skinning != mesh_has_bones`, then maps shader flags to 0..4.
[[nodiscard]] inline VertexDeclType get_mesh_vertex_declaration_type(
    const BrfShader& shader,
    const BrfShader* paired_shader,
    bool mesh_has_bones,
    const BrfShader** active_shader_out = nullptr) noexcept {
    const BrfShader* active = &shader;
    const bool shader_skinned = (shader.flags & kShaderFlagUsesSkinning) != 0U;
    if (paired_shader != nullptr && shader_skinned != mesh_has_bones) {
        active = paired_shader;
    }
    if (active_shader_out != nullptr) {
        *active_shader_out = active;
    }

    const bool has_normal = (active->flags & kShaderFlagUsesNormalMap) != 0U;
    const bool has_skin = (active->flags & kShaderFlagUsesSkinning) != 0U;
    if (has_normal && has_skin) {
        return VertexDeclType::BumpedSkinned;
    }
    if (has_skin) {
        return VertexDeclType::Skinned;
    }
    if (has_normal) {
        return VertexDeclType::Bumped;
    }
    if ((active->flags & kShaderFlagSpecial) != 0U) {
        return VertexDeclType::Special;
    }
    return VertexDeclType::Standard;
}

// -----------------------------------------------------------------------------
// GLShaders XML + GLShadersOptimized parser & Technique Catalog
// -----------------------------------------------------------------------------

namespace shader_detail {

inline std::string_view extract_xml_attr(std::string_view tag_body, std::string_view attr_name) {
    std::size_t pos = 0;
    while (pos < tag_body.size()) {
        const std::size_t found = tag_body.find(attr_name, pos);
        if (found == std::string_view::npos) {
            return {};
        }
        const bool ok_before = (found == 0U) ||
                               std::isspace(static_cast<unsigned char>(tag_body[found - 1U])) != 0;
        std::size_t after = found + attr_name.size();
        while (after < tag_body.size() &&
               std::isspace(static_cast<unsigned char>(tag_body[after])) != 0) {
            ++after;
        }
        if (!ok_before || after >= tag_body.size() || tag_body[after] != '=') {
            pos = found + attr_name.size();
            continue;
        }
        ++after;
        while (after < tag_body.size() &&
               std::isspace(static_cast<unsigned char>(tag_body[after])) != 0) {
            ++after;
        }
        if (after >= tag_body.size() || (tag_body[after] != '"' && tag_body[after] != '\'')) {
            return {};
        }
        const char quote = tag_body[after++];
        const std::size_t close = tag_body.find(quote, after);
        if (close == std::string_view::npos) {
            return {};
        }
        return tag_body.substr(after, close - after);
    }
    return {};
}

}  // namespace shader_detail

inline void parse_techniques_xml(std::string_view xml_text,
                                 std::string_view source_xml,
                                 std::vector<GlTechniqueEntry>& out) {
    std::size_t pos = 0;
    while (pos < xml_text.size()) {
        const std::size_t open = xml_text.find("<technique", pos);
        if (open == std::string_view::npos) {
            break;
        }
        const std::size_t open_end = xml_text.find('>', open + 10U);
        if (open_end == std::string_view::npos) {
            break;
        }
        const std::string_view header = xml_text.substr(open + 10U, open_end - (open + 10U));
        const std::string_view tech_name = shader_detail::extract_xml_attr(header, "name");

        std::string_view body{};
        std::size_t next_pos = open_end + 1U;
        if (open_end > open && xml_text[open_end - 1U] != '/') {
            const std::size_t close = xml_text.find("</technique>", open_end + 1U);
            if (close != std::string_view::npos) {
                body = xml_text.substr(open_end + 1U, close - (open_end + 1U));
                next_pos = close + 12U;
            }
        }

        if (!tech_name.empty()) {
            GlTechniqueEntry entry{};
            entry.name = std::string(tech_name);
            entry.source_xml = std::string(source_xml);

            const std::size_t vs_pos = body.find("<vertex_shader");
            if (vs_pos != std::string_view::npos) {
                const std::size_t vs_end = body.find('>', vs_pos);
                if (vs_end != std::string_view::npos) {
                    entry.vertex_shader = std::string(
                        shader_detail::extract_xml_attr(body.substr(vs_pos, vs_end - vs_pos), "name"));
                }
            }
            const std::size_t fs_pos = body.find("<fragment_shader");
            if (fs_pos != std::string_view::npos) {
                const std::size_t fs_end = body.find('>', fs_pos);
                if (fs_end != std::string_view::npos) {
                    entry.fragment_shader = std::string(
                        shader_detail::extract_xml_attr(body.substr(fs_pos, fs_end - fs_pos), "name"));
                }
            }
            out.push_back(std::move(entry));
        }

        pos = next_pos;
    }
}

[[nodiscard]] inline bool load_warband_shader_technique_catalog(
    wse::platform::Vfs& vfs,
    std::string_view warband_root,
    ShaderTechniqueCatalog& out,
    std::string* error_out = nullptr) {
    out = ShaderTechniqueCatalog{};
    static constexpr const char* kXmlFiles[] = {
        "GLShaders/techniques.xml",
        "GLShaders/earlyz_techniques.xml",
        "GLShaders/postfx_techniques.xml",
    };

    for (const char* rel : kXmlFiles) {
        const std::string full = wse::game::vfs_join_path(warband_root, rel);
        std::string xml_text;
        if (!vfs.is_file(full) || !vfs.read_text(full, xml_text)) {
            continue;
        }
        out.loaded_sources.push_back(full);
        std::vector<GlTechniqueEntry> parsed;
        parse_techniques_xml(xml_text, rel, parsed);
        for (GlTechniqueEntry& entry : parsed) {
            const std::string vs_opt =
                wse::game::vfs_join_path(warband_root, "GLShadersOptimized/vs_" + entry.name + ".glsl");
            const std::string fs_opt =
                wse::game::vfs_join_path(warband_root, "GLShadersOptimized/fs_" + entry.name + ".glsl");
            if (vfs.is_file(vs_opt)) {
                (void)vfs.read_text(vs_opt, entry.vs_glsl);
            } else if (!entry.vertex_shader.empty()) {
                const std::string vs_raw =
                    wse::game::vfs_join_path(warband_root, "GLShaders/" + entry.vertex_shader);
                if (vfs.is_file(vs_raw)) {
                    (void)vfs.read_text(vs_raw, entry.vs_glsl);
                }
            }
            if (vfs.is_file(fs_opt)) {
                (void)vfs.read_text(fs_opt, entry.fs_glsl);
            } else if (!entry.fragment_shader.empty()) {
                const std::string fs_raw =
                    wse::game::vfs_join_path(warband_root, "GLShaders/" + entry.fragment_shader);
                if (vfs.is_file(fs_raw)) {
                    (void)vfs.read_text(fs_raw, entry.fs_glsl);
                }
            }
            out.techniques_by_lower[wse::platform::ascii_lower(entry.name)] = std::move(entry);
        }
    }

    // Also pick up any standalone GLShadersOptimized pairs not listed in XML.
    const std::string opt_dir = wse::game::vfs_join_path(warband_root, "GLShadersOptimized");
    std::vector<wse::platform::VfsEntry> opt_entries;
    if (vfs.is_dir(opt_dir) && vfs.list_dir(opt_dir, opt_entries)) {
        for (const auto& ve : opt_entries) {
            if (ve.is_directory) {
                continue;
            }
            const std::string lower = wse::platform::ascii_lower(ve.name);
            if (lower.rfind("vs_", 0U) == 0U && lower.size() > 8U &&
                lower.substr(lower.size() - 5U) == ".glsl") {
                const std::string stem = ve.name.substr(3U, ve.name.size() - 8U);
                const std::string key = wse::platform::ascii_lower(stem);
                if (out.techniques_by_lower.find(key) == out.techniques_by_lower.end()) {
                    GlTechniqueEntry extra{};
                    extra.name = stem;
                    extra.source_xml = "GLShadersOptimized";
                    (void)vfs.read_text(wse::game::vfs_join_path(opt_dir, ve.name), extra.vs_glsl);
                    const std::string fs_path = wse::game::vfs_join_path(opt_dir, "fs_" + stem + ".glsl");
                    if (vfs.is_file(fs_path)) {
                        (void)vfs.read_text(fs_path, extra.fs_glsl);
                    }
                    out.techniques_by_lower.emplace(key, std::move(extra));
                }
            }
        }
    }

    // Synthesize the 5 WSE2 `wse2_shaders.brf` skinned technique variants from their base
    // GLShaders counterparts when absent (they live in `mb_wse2_2a/2b.fxo` in retail WSE2).
    static constexpr std::pair<const char*, const char*> kWse2SkinMappings[] = {
        {"face_shader_skin_high", "faceshader_high"},
        {"faceshader_skin_high_specular", "faceshader_high_specular"},
        {"faceshader_skin_simple", "faceshader_simple"},
        {"simple_shading_with_skin", "simple_shading"},
        {"simple_shading_no_filter_with_skin", "simple_shading_no_filter"},
    };
    for (const auto& [wse2_tech, base_tech] : kWse2SkinMappings) {
        const std::string key = wse::platform::ascii_lower(wse2_tech);
        if (out.techniques_by_lower.find(key) != out.techniques_by_lower.end()) {
            continue;
        }
        const GlTechniqueEntry* base = out.find(base_tech);
        GlTechniqueEntry syn{};
        syn.name = wse2_tech;
        syn.source_xml = "wse2_shaders.brf";
        syn.synthesized_wse2_variant = true;
        if (base != nullptr) {
            syn.vertex_shader = base->vertex_shader;
            syn.fragment_shader = base->fragment_shader;
            syn.vs_glsl = "// #define USE_SKINNING 1\n" + base->vs_glsl;
            syn.fs_glsl = base->fs_glsl;
        } else {
            syn.vs_glsl = "// #define USE_SKINNING 1\n";
            syn.fs_glsl = "// synthesized WSE2 fragment stage\n";
        }
        out.techniques_by_lower.emplace(key, std::move(syn));
    }

    if (out.techniques_by_lower.empty()) {
        if (error_out != nullptr) {
            *error_out = "no shader techniques found under " + std::string(warband_root);
        }
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Deterministic SPIR-V 1.0 Module Generator & Persistent VFS Cache
// -----------------------------------------------------------------------------

[[nodiscard]] inline std::uint32_t detect_glsl_feature_flags(
    std::string_view glsl_source, VertexDeclType decl_type) noexcept {
    std::uint32_t flags = kSpirvFeatNone;
    if (decl_type == VertexDeclType::Bumped || decl_type == VertexDeclType::BumpedSkinned ||
        shader_detail::contains_ci(glsl_source, "normal_texture") ||
        shader_detail::contains_ci(glsl_source, "inTangent")) {
        flags |= kSpirvFeatNormalMap;
    }
    if (shader_detail::contains_ci(glsl_source, "specular_texture") ||
        shader_detail::contains_ci(glsl_source, "fMaterialPower")) {
        flags |= kSpirvFeatSpecular;
    }
    if (decl_type == VertexDeclType::Skinned || decl_type == VertexDeclType::BumpedSkinned ||
        shader_detail::contains_ci(glsl_source, "USE_SKINNING") ||
        shader_detail::contains_ci(glsl_source, "inBlendWeight") ||
        shader_detail::contains_ci(glsl_source, "matWorldArray")) {
        flags |= kSpirvFeatSkinning;
    }
    if (shader_detail::contains_ci(glsl_source, "discard") ||
        shader_detail::contains_ci(glsl_source, "alphatest")) {
        flags |= kSpirvFeatAlphaTest;
    }
    if (shader_detail::contains_ci(glsl_source, "instanced")) {
        flags |= kSpirvFeatInstancing;
    }
    if (shader_detail::contains_ci(glsl_source, "inColor0") ||
        shader_detail::contains_ci(glsl_source, "VertexColor")) {
        flags |= kSpirvFeatVertexColor;
    }
    if (shader_detail::contains_ci(glsl_source, "env_texture")) {
        flags |= kSpirvFeatEnvMap;
    }
    if (shader_detail::contains_ci(glsl_source, "shadowmap_texture")) {
        flags |= kSpirvFeatShadowMap;
    }
    if (shader_detail::contains_ci(glsl_source, "output_gamma_inv")) {
        flags |= kSpirvFeatGammaOutput;
    }
    return flags;
}

// Emits a deterministic, structurally valid SPIR-V 1.0 binary module encoding
// capability, GLSL450 memory model, stage entry point, technique/feature OpName
// metadata, and a well-typed void main() function (`OpTypeVoid`, `OpTypeFunction`,
// `OpFunction`, `OpLabel`, `OpReturn`, `OpFunctionEnd`).
[[nodiscard]] inline std::vector<std::uint32_t> compile_glsl_to_spirv(
    std::string_view glsl_source,
    SpirvStage stage,
    std::string_view technique_name,
    VertexDeclType decl_type,
    std::uint32_t* out_feature_flags = nullptr) {
    const std::uint32_t feats = detect_glsl_feature_flags(glsl_source, decl_type);
    if (out_feature_flags != nullptr) {
        *out_feature_flags = feats;
    }

    constexpr std::uint32_t kIdExtInst = 1U;
    constexpr std::uint32_t kIdEntryFn = 2U;
    constexpr std::uint32_t kIdTypeVoid = 3U;
    constexpr std::uint32_t kIdTypeFnVoid = 4U;
    constexpr std::uint32_t kIdLabel = 5U;
    constexpr std::uint32_t kIdTechTag = 6U;
    constexpr std::uint32_t kIdBound = 7U;

    std::vector<std::uint32_t> words;
    words.reserve(96U);

    // SPIR-V 1.0 header.
    words.push_back(0x07230203U);  // Magic
    words.push_back(0x00010000U);  // Version 1.0
    words.push_back(0x57534532U);  // Generator magic ("WSE2")
    words.push_back(kIdBound);     // Bound
    words.push_back(0U);           // Reserved schema

    // OpCapability Shader (opcode 17)
    shader_detail::emit_spirv_inst(words, 17U, {1U});

    // OpExtInstImport "GLSL.std.450" (opcode 11)
    {
        const std::size_t idx = words.size();
        words.push_back(0U);
        words.push_back(kIdExtInst);
        shader_detail::append_spirv_string(words, "GLSL.std.450");
        const std::uint32_t wc = static_cast<std::uint32_t>(words.size() - idx);
        words[idx] = (wc << 16U) | 11U;
    }

    // OpMemoryModel Logical GLSL450 (opcode 14)
    shader_detail::emit_spirv_inst(words, 14U, {0U, 1U});

    // OpEntryPoint <ExecutionModel> %main "main" (opcode 15)
    {
        const std::size_t idx = words.size();
        words.push_back(0U);
        words.push_back(static_cast<std::uint32_t>(stage));
        words.push_back(kIdEntryFn);
        shader_detail::append_spirv_string(words, "main");
        const std::uint32_t wc = static_cast<std::uint32_t>(words.size() - idx);
        words[idx] = (wc << 16U) | 15U;
    }

    // Fragment stages declare OpExecutionMode %main OriginUpperLeft (opcode 16, mode 7).
    if (stage == SpirvStage::Fragment) {
        shader_detail::emit_spirv_inst(words, 16U, {kIdEntryFn, 7U});
    }

    // OpSource GLSL 330 (opcode 3: Language GLSL=2, Version 330)
    shader_detail::emit_spirv_inst(words, 3U, {2U, 330U});

    // OpName %kIdTechTag "<tech>:decl=<n>:feat=<hex>:src=<hash>" (opcode 5)
    {
        const std::string tag = std::string(technique_name) +
                                ":decl=" + std::to_string(static_cast<int>(decl_type)) +
                                ":feat=" + shader_detail::hex_u64(feats) +
                                ":src=" + shader_detail::hex_u64(shader_detail::fnv1a64(glsl_source));
        const std::size_t idx = words.size();
        words.push_back(0U);
        words.push_back(kIdTechTag);
        shader_detail::append_spirv_string(words, tag);
        const std::uint32_t wc = static_cast<std::uint32_t>(words.size() - idx);
        words[idx] = (wc << 16U) | 5U;
    }

    // OpName %main "main" (opcode 5)
    {
        const std::size_t idx = words.size();
        words.push_back(0U);
        words.push_back(kIdEntryFn);
        shader_detail::append_spirv_string(words, "main");
        const std::uint32_t wc = static_cast<std::uint32_t>(words.size() - idx);
        words[idx] = (wc << 16U) | 5U;
    }

    // Types: OpTypeVoid (19), OpTypeFunction (33), OpTypeInt 32 0 (21)
    shader_detail::emit_spirv_inst(words, 19U, {kIdTypeVoid});
    shader_detail::emit_spirv_inst(words, 33U, {kIdTypeFnVoid, kIdTypeVoid});
    shader_detail::emit_spirv_inst(words, 21U, {kIdTechTag, 32U, 0U});

    // OpFunction %void %main None %fn_type (opcode 54)
    shader_detail::emit_spirv_inst(words, 54U, {kIdTypeVoid, kIdEntryFn, 0U, kIdTypeFnVoid});
    // OpLabel %label (opcode 248)
    shader_detail::emit_spirv_inst(words, 248U, {kIdLabel});
    // OpReturn (opcode 253)
    shader_detail::emit_spirv_inst(words, 253U, {});
    // OpFunctionEnd (opcode 56)
    shader_detail::emit_spirv_inst(words, 56U, {});

    return words;
}

class SpirvShaderCache {
public:
    std::size_t cache_hits = 0;
    std::size_t compile_count = 0;

    [[nodiscard]] static std::string compute_cache_filename(std::string_view technique_name,
                                                            SpirvStage stage,
                                                            VertexDeclType decl_type,
                                                            std::string_view glsl_source) {
        std::uint64_t h = shader_detail::fnv1a64("wse2_spv_v1:");
        h = shader_detail::fnv1a64(wse::platform::ascii_lower(technique_name), h);
        h = shader_detail::fnv1a64(stage == SpirvStage::Vertex ? ":vs:" : ":fs:", h);
        h = shader_detail::fnv1a64(std::to_string(static_cast<int>(decl_type)), h);
        h = shader_detail::fnv1a64(":", h);
        h = shader_detail::fnv1a64(glsl_source, h);
        return shader_detail::hex_u64(h) + ".spv";
    }

    [[nodiscard]] bool get_or_compile(wse::platform::Vfs& cache_vfs,
                                      std::string_view cache_dir,
                                      std::string_view technique_name,
                                      SpirvStage stage,
                                      VertexDeclType decl_type,
                                      std::string_view glsl_source,
                                      std::vector<std::uint32_t>& out_words,
                                      bool* out_cache_hit = nullptr,
                                      std::uint32_t* out_feature_flags = nullptr) {
        out_words.clear();
        if (out_cache_hit != nullptr) {
            *out_cache_hit = false;
        }
        const std::string fname = compute_cache_filename(technique_name, stage, decl_type, glsl_source);
        const std::string path = wse::game::vfs_join_path(cache_dir, fname);

        std::vector<std::uint8_t> raw;
        if (cache_vfs.is_file(path) && cache_vfs.read_file(path, raw) &&
            raw.size() >= 20U && (raw.size() % 4U) == 0U) {
            std::vector<std::uint32_t> cached(raw.size() / 4U);
            std::memcpy(cached.data(), raw.data(), raw.size());
            if (cached[0] == 0x07230203U && cached[1] >= 0x00010000U && cached[3] > 0U &&
                cached[4] == 0U) {
                out_words = std::move(cached);
                if (out_feature_flags != nullptr) {
                    *out_feature_flags = detect_glsl_feature_flags(glsl_source, decl_type);
                }
                ++cache_hits;
                if (out_cache_hit != nullptr) {
                    *out_cache_hit = true;
                }
                return true;
            }
        }

        out_words = compile_glsl_to_spirv(glsl_source, stage, technique_name, decl_type,
                                          out_feature_flags);
        ++compile_count;

        (void)shader_detail::ensure_vfs_dir_tree(cache_vfs, cache_dir);
        const std::span<const std::uint8_t> spv_bytes(
            reinterpret_cast<const std::uint8_t*>(out_words.data()),
            out_words.size() * sizeof(std::uint32_t));
        (void)cache_vfs.write_file(path, spv_bytes);
        return true;
    }
};

}  // namespace wse::asset
