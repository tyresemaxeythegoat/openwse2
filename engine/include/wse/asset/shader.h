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
//
// Port infrastructure in this header that is NOT a reconstruction:
// - `lower_warband_glsl_to_vulkan450` lowers legacy Warband GLSL 1.10/1.20 (`attribute`,
//   `varying`, loose `uniform`, `gl_FragColor`, `gl_FragData`, `texture2D`, `shadow2D`)
//   into Vulkan `#version 450` GLSL using canonical vertex attribute locations, varying
//   locations and sampler bindings shared by both stages of a technique, per-stage
//   `std140` UBOs (binding 0 VS / binding 1 FS), and sampler bindings from 2.
// - `SpirvCompilerBackend` + `SpirvShaderCache` (persistent VFS cache),
//   `detect_glsl_feature_flags`, and the synthesized WSE2 skinning techniques have no
//   retail counterpart. Real SPIR-V 1.0 emission is implemented behind
//   `wse/asset/shader_compiler.h` (`wse_shader` / Khronos `glslang`).
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
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
// rglMesh::getVertexDeclarationType 0x1402baa30, rglRenderer::renderMesh 0x1402e8454,
// rglResourceManager::initShaderPairs 0x1402ed3c0).
inline constexpr std::uint32_t kShaderFlagHasSpecular = 0x00000020U;
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

// rglResourceManager::getSuitableShader (0x1402f3db0) starts its best depth at 0x186a0.
inline constexpr int kSuitableShaderNoDepth = 100000;

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
    // Port heuristic, not retail evidence: the diffuse slot is sampled as sRGB
    // whenever it names a texture.
    bool srgb_diffuse = true;
    bool uses_normal_map = false;
    bool uses_specular = false;
    bool uses_skinning = false;
    MaterialBlendMode blend_mode = MaterialBlendMode::Opaque;
    AlphaTestMode alpha_test_mode = AlphaTestMode::None;
    std::uint8_t alpha_ref_u8 = 0;
    // Provisional: bits 24..27 decoded as a signed nibble (-8..7). The sign
    // convention is not yet confirmed against rglRenderer::renderMesh (0x1402e7f50).
    int render_order = 0;
    std::string diffuse_a{};
    std::string diffuse_b{};
    std::string bumpmap{};
    std::string envmap{};
    std::string specular_map{};
    float specular_coeff = 0.0f;
    Vec3f specular_rgb{};
};

// Values equal SPIR-V `ExecutionModel` (Vertex = 0, Fragment = 4).
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

struct TechniqueCatalogOptions {
    // False loads technique names and XML metadata only (no GLSL reads), for
    // callers that only need the set of available techniques.
    bool load_glsl_sources = true;
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

inline std::string_view strip_utf8_bom(std::string_view s) noexcept {
    if (s.size() >= 3U && static_cast<unsigned char>(s[0]) == 0xEFU &&
        static_cast<unsigned char>(s[1]) == 0xBBU && static_cast<unsigned char>(s[2]) == 0xBFU) {
        s.remove_prefix(3U);
    }
    return s;
}

inline bool equals_ci(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (wse::platform::ascii_lower(a[i]) != wse::platform::ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

// Non-allocating ASCII case-insensitive search.
inline std::size_t find_ci(std::string_view haystack,
                           std::string_view needle,
                           std::size_t from = 0U) noexcept {
    if (from > haystack.size()) {
        return std::string_view::npos;
    }
    if (needle.empty()) {
        return from;
    }
    if (haystack.size() < needle.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        if (equals_ci(haystack.substr(i, needle.size()), needle)) {
            return i;
        }
    }
    return std::string_view::npos;
}

inline bool contains_ci(std::string_view haystack, std::string_view needle) noexcept {
    return find_ci(haystack, needle) != std::string_view::npos;
}

inline constexpr bool is_ident_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_';
}

inline constexpr bool is_inline_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

// Calls `fn(identifier)` for every identifier token of GLSL source that lies outside
// comments. Preprocessor lines contribute only the macro name of `#define NAME`, so
// `#ifdef USE_SKINNING` or a commented-out `// #define USE_SKINNING` never counts.
template <typename Fn>
inline void for_each_glsl_identifier(std::string_view src, Fn&& fn) noexcept {
    const std::size_t n = src.size();
    std::size_t i = 0;
    bool line_start = true;
    while (i < n) {
        const char c = src[i];
        if (c == '/' && i + 1U < n && src[i + 1U] == '/') {
            while (i < n && src[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (c == '/' && i + 1U < n && src[i + 1U] == '*') {
            const std::size_t end = src.find("*/", i + 2U);
            i = (end == std::string_view::npos) ? n : end + 2U;
            continue;
        }
        if (c == '\n') {
            line_start = true;
            ++i;
            continue;
        }
        if (is_inline_space(c)) {
            ++i;
            continue;
        }
        if (c == '#' && line_start) {
            std::size_t j = i + 1U;
            while (j < n && is_inline_space(src[j])) {
                ++j;
            }
            const std::size_t dir_begin = j;
            while (j < n && is_ident_char(src[j])) {
                ++j;
            }
            if (src.substr(dir_begin, j - dir_begin) == "define") {
                while (j < n && is_inline_space(src[j])) {
                    ++j;
                }
                const std::size_t name_begin = j;
                while (j < n && is_ident_char(src[j])) {
                    ++j;
                }
                if (j > name_begin) {
                    fn(src.substr(name_begin, j - name_begin));
                }
            }
            while (j < n && src[j] != '\n') {
                j += (src[j] == '\\' && j + 1U < n && src[j + 1U] == '\n') ? 2U : 1U;
            }
            i = j;
            line_start = false;
            continue;
        }
        line_start = false;
        if (is_ident_char(c)) {
            const std::size_t begin = i;
            while (i < n && is_ident_char(src[i])) {
                ++i;
            }
            if (!(src[begin] >= '0' && src[begin] <= '9')) {
                fn(src.substr(begin, i - begin));
            }
            continue;
        }
        ++i;
    }
}

inline std::string normalize_material_slot(std::string_view raw) {
    const std::string_view trimmed = trim_ascii(raw);
    if (trimmed.empty()) {
        return {};
    }
    // rglResourceManager::initMaterialTextures (0x1402f5390) ignores "none".
    if (equals_ci(trimmed, "none")) {
        return {};
    }
    return std::string(trimmed);
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

// Decodes a NUL-terminated SPIR-V literal string; nullopt when no NUL is present.
inline std::optional<std::string> read_spirv_string(std::span<const std::uint32_t> words) {
    std::string out;
    for (const std::uint32_t w : words) {
        for (std::uint32_t b = 0; b < 4U; ++b) {
            const char c = static_cast<char>((w >> (8U * b)) & 0xFFU);
            if (c == '\0') {
                return out;
            }
            out.push_back(c);
        }
    }
    return std::nullopt;
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

inline std::optional<std::uint64_t> parse_hex_u64(std::string_view s) noexcept {
    if (s.empty() || s.size() > 16U) {
        return std::nullopt;
    }
    std::uint64_t v = 0;
    for (const char c : s) {
        std::uint64_t d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<std::uint64_t>(c - 'a' + 10);
        } else {
            return std::nullopt;
        }
        v = (v << 4U) | d;
    }
    return v;
}

// Creates every missing directory of `dir`. An intermediate component that cannot
// be created or probed (a drive or mount root) is skipped; only the final directory
// must exist afterwards.
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
            (void)vfs.make_dir(prefix);
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

// `read_text` that never leaves a partial or stale value behind on failure.
inline bool read_text_or_clear(wse::platform::Vfs& vfs, std::string_view path, std::string& out) {
    if (vfs.read_text(path, out)) {
        return true;
    }
    out.clear();
    return false;
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

// Float threshold for shaders comparing a [0, 1] alpha: `ref / 256` sits between
// (ref - 1) / 255 and ref / 255, so 8-bit inputs pass exactly when the D3D9
// integer test `alpha >= ref` passes.
[[nodiscard]] inline constexpr float alpha_test_ref_float(AlphaTestMode mode) noexcept {
    return static_cast<float>(alpha_test_ref_u8(mode)) / 256.0f;
}

[[nodiscard]] inline MaterialRenderState decode_material_render_state(
    const BrfMaterial& mat, const BrfShader* resolved_shader = nullptr) {
    MaterialRenderState out{};
    out.no_fog = (mat.flags & kMaterialFlagNoFog) != 0U;
    out.depth_write = (mat.flags & kMaterialFlagNoZWrite) == 0U;
    out.depth_test = (mat.flags & kMaterialFlagNoDepthTest) == 0U;

    // `rglGraphicsContext::createBlendStates` (`0x14029f590`) allocates 5 slots (`0..4`).
    // Values 5..6 have no slot; value 7 (`0x700`) is resolved dynamically per draw in
    // `rglRenderer::renderMesh` (`0x1402e913f`) / `rglMesh::updateRenderOrder` (`0x1402b9ece`),
    // selecting slot 1 (`AlphaBlend`) when mesh vertex-color alpha < 255, else slot 0 (`Opaque`).
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
    // `rglRenderer::renderMesh` (`0x1402e8439`) -> `rglGraphicsContext::setAlphaTestMode`
    // (`0x1402a00b0`): `(mat.flags & 0x700) != 0` only zeroes the shader's `fAlphaTestVal`
    // uniform; `D3DRS_ALPHATESTENABLE` and `D3DRS_ALPHAREF` (`8`/`136`/`251` at `0x1402a0165`)
    // stay active whenever `alpha_test_mode != None`, even on blended materials.
    out.alpha_ref_u8 = alpha_test_ref_u8(out.alpha_test_mode);

    const std::uint32_t raw_order = (mat.flags & kMaterialFlagRenderOrderMask) >> 24U;
    // Signed 4-bit nibble `-8..+7`: `rglMesh::updateRenderOrder` (`0x1402b9ef5`) computes
    // `((flags >> 24) - 8) & 0xF` (which equals `render_order + 8` unsigned sort key).
    out.render_order = (raw_order >= 8U) ? static_cast<int>(raw_order) - 16 : static_cast<int>(raw_order);

    out.diffuse_a = shader_detail::normalize_material_slot(mat.texture_names[0]);
    out.diffuse_b = shader_detail::normalize_material_slot(mat.texture_names[1]);
    out.bumpmap = shader_detail::normalize_material_slot(mat.texture_names[2]);
    out.envmap = shader_detail::normalize_material_slot(mat.texture_names[3]);
    out.specular_map = shader_detail::normalize_material_slot(mat.texture_names[4]);

    out.specular_coeff = mat.specular_coeff;
    out.specular_rgb = mat.specular_rgb;
    out.srgb_diffuse = !out.diffuse_a.empty();

    const bool material_wants_specular =
        !out.specular_map.empty() ||
        (mat.specular_coeff > 0.0f &&
         (mat.specular_rgb.x > 0.0f || mat.specular_rgb.y > 0.0f || mat.specular_rgb.z > 0.0f));
    out.uses_normal_map = !out.bumpmap.empty();
    if (resolved_shader != nullptr) {
        out.uses_normal_map =
            out.uses_normal_map || (resolved_shader->flags & kShaderFlagUsesNormalMap) != 0U;
        out.uses_skinning = (resolved_shader->flags & kShaderFlagUsesSkinning) != 0U;
        // `rglRenderer::renderMesh` (`0x1402e8454`) gates `fMaterialPower` / `vSpecularColor`
        // on `shader->m_flags & kShaderFlagHasSpecular` (`0x20`).
        out.uses_specular =
            material_wants_specular && ((resolved_shader->flags & kShaderFlagHasSpecular) != 0U);
    } else {
        out.uses_specular = material_wants_specular;
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

// True when effect source already carries a WSE2 conditional: a preprocessor
// `#if WSE2`, `#ifdef WSE2`, `#elif WSE2`, or `#if defined(WSE2)` line with `WSE2`
// as a whole token. Safety deviation: retail checkLoadingPostEffects (0x14029c930)
// is treated as a substring check, which would duplicate the include for `#ifdef
// WSE2` and skip it for `#if WSE2_LEGACY`.
[[nodiscard]] inline bool effect_source_has_wse2_conditional(std::string_view src) noexcept {
    using shader_detail::is_ident_char;
    using shader_detail::is_inline_space;
    src = shader_detail::strip_utf8_bom(src);
    std::size_t pos = 0;
    while (pos < src.size()) {
        const std::size_t eol = src.find('\n', pos);
        std::string_view line = src.substr(pos, (eol == std::string_view::npos) ? src.size() - pos
                                                                             : eol - pos);
        pos = (eol == std::string_view::npos) ? src.size() : eol + 1U;
        std::size_t i = 0;
        while (i < line.size() && is_inline_space(line[i])) {
            ++i;
        }
        if (i >= line.size() || line[i] != '#') {
            continue;
        }
        ++i;
        const auto skip_ws = [&]() {
            while (i < line.size() && is_inline_space(line[i])) {
                ++i;
            }
        };
        const auto read_ident = [&]() {
            const std::size_t b = i;
            while (i < line.size() && is_ident_char(line[i])) {
                ++i;
            }
            return line.substr(b, i - b);
        };
        skip_ws();
        const std::string_view directive = read_ident();
        if (directive != "if" && directive != "ifdef" && directive != "elif") {
            continue;
        }
        skip_ws();
        std::string_view token = read_ident();
        if (directive != "ifdef" && token == "defined") {
            skip_ws();
            if (i < line.size() && line[i] == '(') {
                ++i;
                skip_ws();
            }
            token = read_ident();
        }
        if (token == "WSE2") {
            return true;
        }
    }
    return false;
}

// Matches rglGraphicsContext::checkLoadingPostEffects (0x14029c930):
//   Checks <module>/PostFx.fx (or <module>/Shaders/postFX.fx, then root PostFx.fx),
//   appends `#if WSE2\n#include "postFX_WSE2.fx"\n#endif\n` when `use_wse2_effect` is set
//   and no WSE2 conditional is present, and flags if <module>/postFX_WSE2.fx shadows root.
//   The Vfs is case-insensitive, so one probe per location covers `postFX.fx` spellings.
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
             wse::game::vfs_join_path(module_dir, "Shaders/postFX.fx")});
        out.module_wse2_shadows_root =
            vfs.is_file(wse::game::vfs_join_path(module_dir, "postFX_WSE2.fx")) &&
            vfs.is_file(wse::game::vfs_join_path(warband_root, "postFX_WSE2.fx"));
    }
    if (out.resolved_path.empty()) {
        out.resolved_path = shader_detail::probe_first_existing(
            vfs, {wse::game::vfs_join_path(warband_root, "PostFx.fx")});
    }
    if (out.resolved_path.empty() || !vfs.read_text(out.resolved_path, out.source_text)) {
        out.source_text.clear();
        if (error_out != nullptr) {
            *error_out = "unable to locate PostFx.fx";
        }
        return false;
    }
    if (use_wse2_effect && !effect_source_has_wse2_conditional(out.source_text)) {
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

// Parses `[SkinFallback]` / `[NoSkinFallback]` pairs. A leading UTF-8 BOM is
// ignored. A key repeated within one section keeps its first position in `entries`
// but takes the later value everywhere (with a warning), so `entries` and the maps
// never disagree.
inline void parse_wse2_shaders_ini(std::string_view ini_text, ShaderPairTable& out) {
    out = ShaderPairTable{};
    enum class Section : std::uint8_t { Other, SkinFallback, NoSkinFallback };
    Section sec = Section::Other;
    std::unordered_map<std::string, std::size_t> skin_index;
    std::unordered_map<std::string, std::size_t> noskin_index;

    ini_text = shader_detail::strip_utf8_bom(ini_text);
    std::size_t pos = 0;
    while (pos < ini_text.size()) {
        const std::size_t eol = ini_text.find('\n', pos);
        std::string_view line = (eol == std::string_view::npos)
                                    ? ini_text.substr(pos)
                                    : ini_text.substr(pos, eol - pos);
        pos = (eol == std::string_view::npos) ? ini_text.size() : eol + 1U;

        // Strip ';' or '#' comments before trimming whitespace.
        const std::size_t comment = line.find_first_of(";#");
        if (comment != std::string_view::npos) {
            line = line.substr(0, comment);
        }
        line = shader_detail::trim_ascii(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            const std::string_view sec_name =
                shader_detail::trim_ascii(line.substr(1U, line.size() - 2U));
            if (shader_detail::equals_ci(sec_name, "skinfallback")) {
                sec = Section::SkinFallback;
            } else if (shader_detail::equals_ci(sec_name, "noskinfallback")) {
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
        std::string key = wse::platform::ascii_lower(lhs);
        auto& index = entry.skin_fallback ? skin_index : noskin_index;
        auto& map = entry.skin_fallback ? out.skin_fallback_map : out.noskin_fallback_map;
        map[key] = entry.target_shader;
        const auto found = index.find(key);
        if (found != index.end()) {
            out.warnings.push_back("duplicate wse2_shaders.ini key, later value wins: " +
                                   entry.source_shader);
            out.entries[found->second] = std::move(entry);
        } else {
            index.emplace(std::move(key), out.entries.size());
            out.entries.push_back(std::move(entry));
        }
    }
}

// -----------------------------------------------------------------------------
// Shader Suitability, Alternative Resolution & Vertex Decl Type
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

namespace shader_detail {

inline bool technique_available(const BrfShader& sh,
                                const std::unordered_set<std::string>* available_techniques_lower) {
    return (available_techniques_lower == nullptr) ||
           (available_techniques_lower->find(wse::platform::ascii_lower(sh.technique_name)) !=
            available_techniques_lower->end());
}

}  // namespace shader_detail

inline void validate_shader_pairs(const ShaderPairTable& raw_table,
                                  const std::unordered_map<std::string, BrfShader>& shaders_by_lower,
                                  const ShaderQualityConfig& quality_cfg,
                                  ShaderPairTable& validated_out,
                                  const std::unordered_set<std::string>* available_techniques_lower = nullptr) {
    validated_out = ShaderPairTable{};
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
        if (!is_shader_suitable(dst, quality_cfg,
                                shader_detail::technique_available(dst, available_techniques_lower))) {
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

// Reconstructs rglResourceManager::getSuitableShader (0x1402f3db0) ->
// getSuitableShaderImpl (0x1402f3eb0) -> getDefaultShader ("def_shader", 0x1402f1ef0):
// the suitable shader reachable through the fewest alternative hops, else
// `def_shader`. Implemented as a breadth-first search with one global visited set,
// O(shaders + alternatives) even for cyclic or densely linked alternative graphs
// (a path-local depth-first search is exponential there). Among equally shallow
// candidates the first in alternative-list order wins.
[[nodiscard]] inline ShaderResolutionResult resolve_suitable_shader(
    std::string_view requested_name,
    const std::unordered_map<std::string, BrfShader>& shaders_by_lower,
    const ShaderQualityConfig& cfg = {},
    const std::unordered_set<std::string>* available_techniques_lower = nullptr) {
    std::deque<std::pair<std::string, int>> queue;
    std::unordered_set<std::string> seen;
    std::string start = wse::platform::ascii_lower(requested_name);
    if (!start.empty()) {
        seen.insert(start);
        queue.emplace_back(std::move(start), 0);
    }
    while (!queue.empty()) {
        const std::pair<std::string, int> node = std::move(queue.front());
        queue.pop_front();
        const auto it = shaders_by_lower.find(node.first);
        if (it == shaders_by_lower.end()) {
            continue;
        }
        const BrfShader& sh = it->second;
        if (is_shader_suitable(sh, cfg,
                               shader_detail::technique_available(sh, available_techniques_lower))) {
            return ShaderResolutionResult{&sh, node.second, false};
        }
        if (node.second + 1 >= kSuitableShaderNoDepth) {
            continue;
        }
        for (const std::string& alt : sh.alternatives) {
            std::string key = wse::platform::ascii_lower(alt);
            if (!key.empty() && seen.insert(key).second) {
                queue.emplace_back(std::move(key), node.second + 1);
            }
        }
    }

    const auto def_it = shaders_by_lower.find("def_shader");
    if (def_it != shaders_by_lower.end()) {
        return ShaderResolutionResult{&def_it->second, kSuitableShaderNoDepth, true};
    }
    return ShaderResolutionResult{nullptr, kSuitableShaderNoDepth, true};
}

// Memoizing front end for `resolve_suitable_shader`: materials share a small set
// of shaders, so each distinct (case-folded) name is resolved once. The shader map
// and technique set must outlive the resolver and stay unmodified.
class SuitableShaderResolver {
public:
    explicit SuitableShaderResolver(
        const std::unordered_map<std::string, BrfShader>& shaders_by_lower,
        ShaderQualityConfig cfg = {},
        const std::unordered_set<std::string>* available_techniques_lower = nullptr)
        : shaders_(&shaders_by_lower), cfg_(cfg), techniques_(available_techniques_lower) {}

    [[nodiscard]] const ShaderResolutionResult& resolve(std::string_view name) {
        std::string key = wse::platform::ascii_lower(name);
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }
        ShaderResolutionResult res = resolve_suitable_shader(key, *shaders_, cfg_, techniques_);
        return cache_.emplace(std::move(key), res).first->second;
    }

    [[nodiscard]] std::size_t cached_count() const noexcept { return cache_.size(); }

private:
    const std::unordered_map<std::string, BrfShader>* shaders_;
    ShaderQualityConfig cfg_;
    const std::unordered_set<std::string>* techniques_;
    std::unordered_map<std::string, ShaderResolutionResult> cache_{};
};

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

// Index of the `>` that closes a tag whose attributes start at `from`; a `>` inside a
// quoted attribute value does not count. npos when unterminated.
inline std::size_t find_xml_tag_close(std::string_view xml, std::size_t from) noexcept {
    char quote = 0;
    for (std::size_t i = from; i < xml.size(); ++i) {
        const char c = xml[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            return i;
        }
    }
    return std::string_view::npos;
}

// True when `xml[lt]` opens `<name` (or `</name` with `closing`) followed by a tag
// boundary, so `<technique` never matches `<techniques>`.
inline bool xml_tag_name_at(std::string_view xml,
                            std::size_t lt,
                            std::string_view name,
                            bool closing) noexcept {
    std::size_t p = lt + 1U;
    if (closing) {
        if (p >= xml.size() || xml[p] != '/') {
            return false;
        }
        ++p;
    }
    if (p + name.size() >= xml.size() || xml.compare(p, name.size(), name) != 0) {
        return false;
    }
    const char c = xml[p + name.size()];
    return c == '>' || c == '/' || std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Next `<` at or after `pos` that is not inside a comment, CDATA section, or
// processing instruction.
inline std::size_t next_xml_tag(std::string_view xml, std::size_t pos) noexcept {
    while (pos < xml.size()) {
        const std::size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) {
            return lt;
        }
        std::string_view terminator{};
        std::size_t skip = 0;
        if (xml.compare(lt, 4U, "<!--") == 0) {
            terminator = "-->";
            skip = 4U;
        } else if (xml.compare(lt, 9U, "<![CDATA[") == 0) {
            terminator = "]]>";
            skip = 9U;
        } else if (xml.compare(lt, 2U, "<?") == 0) {
            terminator = "?>";
            skip = 2U;
        } else {
            return lt;
        }
        const std::size_t end = xml.find(terminator, lt + skip);
        if (end == std::string_view::npos) {
            return std::string_view::npos;
        }
        pos = end + terminator.size();
    }
    return std::string_view::npos;
}

struct XmlElement {
    std::string_view attrs{};
    std::string_view inner{};
    std::size_t end = 0;  // index just past the element
};

// Finds the next `<name ...>` element at or after `from`. `inner` spans up to the
// first matching close tag (elements of the same name are not nested in GLShaders).
// An unterminated element is returned with an empty body.
inline std::optional<XmlElement> find_xml_element(std::string_view xml,
                                                  std::string_view name,
                                                  std::size_t from) noexcept {
    std::size_t pos = from;
    for (;;) {
        const std::size_t lt = next_xml_tag(xml, pos);
        if (lt == std::string_view::npos) {
            return std::nullopt;
        }
        if (!xml_tag_name_at(xml, lt, name, false)) {
            pos = lt + 1U;
            continue;
        }
        const std::size_t attrs_begin = lt + 1U + name.size();
        const std::size_t gt = find_xml_tag_close(xml, attrs_begin);
        if (gt == std::string_view::npos) {
            return std::nullopt;
        }
        XmlElement el{};
        el.attrs = xml.substr(attrs_begin, gt - attrs_begin);
        const std::string_view trimmed = trim_ascii(el.attrs);
        el.end = gt + 1U;
        if (!trimmed.empty() && trimmed.back() == '/') {
            el.attrs = trimmed.substr(0U, trimmed.size() - 1U);
            return el;
        }
        std::size_t scan = gt + 1U;
        for (;;) {
            const std::size_t close = next_xml_tag(xml, scan);
            if (close == std::string_view::npos) {
                return el;
            }
            if (xml_tag_name_at(xml, close, name, true)) {
                el.inner = xml.substr(gt + 1U, close - (gt + 1U));
                const std::size_t close_gt = xml.find('>', close);
                el.end = (close_gt == std::string_view::npos) ? xml.size() : close_gt + 1U;
                return el;
            }
            scan = close + 1U;
        }
    }
}

// `<child name="x"/>` or `<child>x</child>`.
inline std::string xml_child_value(std::string_view parent_inner, std::string_view child) {
    const std::optional<XmlElement> el = find_xml_element(parent_inner, child, 0U);
    if (!el) {
        return {};
    }
    const std::string_view attr = extract_xml_attr(el->attrs, "name");
    if (!attr.empty()) {
        return std::string(attr);
    }
    return std::string(trim_ascii(el->inner));
}

// Inserts `#define <define_body>` right after a leading `#version` line (which GLSL
// requires to come first), else at the top.
inline std::string inject_glsl_define(std::string_view glsl, std::string_view define_body) {
    const std::string line = "#define " + std::string(define_body) + "\n";
    std::size_t pos = 0;
    while (pos < glsl.size()) {
        const std::size_t eol = glsl.find('\n', pos);
        const std::string_view l = trim_ascii(
            glsl.substr(pos, (eol == std::string_view::npos) ? glsl.size() - pos : eol - pos));
        if (l.rfind("#version", 0U) == 0U) {
            const std::size_t insert_at = (eol == std::string_view::npos) ? glsl.size() : eol + 1U;
            std::string out(glsl.substr(0U, insert_at));
            if (eol == std::string_view::npos) {
                out.push_back('\n');
            }
            out += line;
            out.append(glsl.substr(insert_at));
            return out;
        }
        if (!l.empty() && l.rfind("//", 0U) != 0U) {
            break;  // first non-comment line is not #version
        }
        pos = (eol == std::string_view::npos) ? glsl.size() : eol + 1U;
    }
    return line + std::string(glsl);
}

}  // namespace shader_detail

inline void parse_techniques_xml(std::string_view xml_text,
                                 std::string_view source_xml,
                                 std::vector<GlTechniqueEntry>& out) {
    std::size_t pos = 0;
    while (const std::optional<shader_detail::XmlElement> el =
               shader_detail::find_xml_element(xml_text, "technique", pos)) {
        pos = el->end;
        const std::string_view tech_name = shader_detail::extract_xml_attr(el->attrs, "name");
        if (tech_name.empty()) {
            continue;
        }
        GlTechniqueEntry entry{};
        entry.name = std::string(tech_name);
        entry.source_xml = std::string(source_xml);
        entry.vertex_shader = shader_detail::xml_child_value(el->inner, "vertex_shader");
        entry.fragment_shader = shader_detail::xml_child_value(el->inner, "fragment_shader");
        out.push_back(std::move(entry));
    }
}

// Loads `GLShaders/*.xml` techniques (+ their `GLShadersOptimized` or `GLShaders`
// GLSL when `opts.load_glsl_sources`) and standalone `GLShadersOptimized/vs_*.glsl`
// pairs. False (with an error) when no technique is found at all. The 5 WSE2
// skinning variants are then synthesized from their base techniques, only when the
// base exists and the variant is absent.
[[nodiscard]] inline bool load_warband_shader_technique_catalog(
    wse::platform::Vfs& vfs,
    std::string_view warband_root,
    ShaderTechniqueCatalog& out,
    std::string* error_out = nullptr,
    const TechniqueCatalogOptions& opts = {}) {
    out = ShaderTechniqueCatalog{};
    static constexpr const char* kXmlFiles[] = {
        "GLShaders/techniques.xml",
        "GLShaders/earlyz_techniques.xml",
        "GLShaders/postfx_techniques.xml",
    };

    std::string xml_text;
    std::vector<GlTechniqueEntry> parsed;
    for (const char* rel : kXmlFiles) {
        const std::string full = wse::game::vfs_join_path(warband_root, rel);
        if (!vfs.read_text(full, xml_text)) {
            continue;
        }
        out.loaded_sources.push_back(full);
        parsed.clear();
        parse_techniques_xml(xml_text, rel, parsed);
        for (GlTechniqueEntry& entry : parsed) {
            if (opts.load_glsl_sources) {
                const std::string vs_opt = wse::game::vfs_join_path(
                    warband_root, "GLShadersOptimized/vs_" + entry.name + ".glsl");
                const std::string fs_opt = wse::game::vfs_join_path(
                    warband_root, "GLShadersOptimized/fs_" + entry.name + ".glsl");
                if (!shader_detail::read_text_or_clear(vfs, vs_opt, entry.vs_glsl) &&
                    !entry.vertex_shader.empty()) {
                    (void)shader_detail::read_text_or_clear(
                        vfs, wse::game::vfs_join_path(warband_root, "GLShaders/" + entry.vertex_shader),
                        entry.vs_glsl);
                }
                if (!shader_detail::read_text_or_clear(vfs, fs_opt, entry.fs_glsl) &&
                    !entry.fragment_shader.empty()) {
                    (void)shader_detail::read_text_or_clear(
                        vfs, wse::game::vfs_join_path(warband_root, "GLShaders/" + entry.fragment_shader),
                        entry.fs_glsl);
                }
            }
            out.techniques_by_lower[wse::platform::ascii_lower(entry.name)] = std::move(entry);
        }
    }

    // Also pick up any standalone GLShadersOptimized pairs not listed in XML.
    const std::string opt_dir = wse::game::vfs_join_path(warband_root, "GLShadersOptimized");
    std::vector<wse::platform::VfsEntry> opt_entries;
    if (vfs.list_dir(opt_dir, opt_entries)) {
        for (const auto& ve : opt_entries) {
            if (ve.is_directory || ve.name.size() <= 8U) {
                continue;
            }
            const std::string_view name(ve.name);
            if (!shader_detail::equals_ci(name.substr(0U, 3U), "vs_") ||
                !shader_detail::equals_ci(name.substr(name.size() - 5U), ".glsl")) {
                continue;
            }
            const std::string stem(name.substr(3U, name.size() - 8U));
            std::string key = wse::platform::ascii_lower(stem);
            if (out.techniques_by_lower.find(key) != out.techniques_by_lower.end()) {
                continue;
            }
            GlTechniqueEntry extra{};
            extra.name = stem;
            extra.source_xml = "GLShadersOptimized";
            if (opts.load_glsl_sources) {
                (void)shader_detail::read_text_or_clear(
                    vfs, wse::game::vfs_join_path(opt_dir, ve.name), extra.vs_glsl);
                (void)shader_detail::read_text_or_clear(
                    vfs, wse::game::vfs_join_path(opt_dir, "fs_" + stem + ".glsl"), extra.fs_glsl);
            }
            out.techniques_by_lower.emplace(std::move(key), std::move(extra));
        }
    }

    if (out.techniques_by_lower.empty()) {
        if (error_out != nullptr) {
            *error_out = "no shader techniques found under " + std::string(warband_root);
        }
        return false;
    }

    // Synthesize the 5 WSE2 `wse2_shaders.brf` skinned technique variants from their base
    // GLShaders counterparts when absent (they live in `mb_wse2_2a/2b.fxo` in retail WSE2).
    // The vertex stage gets a real `#define USE_SKINNING 1`; it only skins if the base
    // GLSL branches on that macro.
    static constexpr std::pair<const char*, const char*> kWse2SkinMappings[] = {
        {"face_shader_skin_high", "face_shader_high"},
        {"faceshader_skin_high_specular", "faceshader_high_specular"},
        {"faceshader_skin_simple", "faceshader_simple"},
        {"simple_shading_with_skin", "simple_shading"},
        {"simple_shading_no_filter_with_skin", "simple_shading_no_filter"},
    };
    for (const auto& [wse2_tech, base_tech] : kWse2SkinMappings) {
        std::string key = wse::platform::ascii_lower(wse2_tech);
        if (out.techniques_by_lower.find(key) != out.techniques_by_lower.end()) {
            continue;
        }
        const GlTechniqueEntry* base = out.find(base_tech);
        if (base == nullptr) {
            out.warnings.push_back(std::string("WSE2 skinning variant ") + wse2_tech +
                                   " not synthesized: base technique " + base_tech + " missing");
            continue;
        }
        GlTechniqueEntry syn{};
        syn.name = wse2_tech;
        syn.source_xml = "wse2_shaders.brf";
        syn.synthesized_wse2_variant = true;
        syn.vertex_shader = base->vertex_shader;
        syn.fragment_shader = base->fragment_shader;
        syn.vs_glsl = shader_detail::inject_glsl_define(base->vs_glsl, "USE_SKINNING 1");
        syn.fs_glsl = base->fs_glsl;
        out.techniques_by_lower.emplace(std::move(key), std::move(syn));
    }
    return true;
}

// -----------------------------------------------------------------------------
// Warband GLSL -> Vulkan 450 Lowering, SPIR-V 1.0 Compiler Interface & VFS Cache
// -----------------------------------------------------------------------------

// Bumped whenever lowering/code-generation changes; stored inside every module's
// `OpString` tag (`wse2:...gen=4`) and baked into every cache filename so stale
// or placeholder `.spv` binaries are automatically rejected and recompiled.
inline constexpr std::uint32_t kWseSpirvGeneratorVersion = 4U;

namespace shader_detail {

inline constexpr std::uint32_t kLegacyPlaceholderSpirvMagic = 0x57534532U;  // "WSE2"

struct GlslFeatureToken {
    std::string_view token;
    std::uint32_t flag;
};

inline constexpr GlslFeatureToken kGlslFeatureTokens[] = {
    {"normal_texture", kSpirvFeatNormalMap},
    {"inTangent", kSpirvFeatNormalMap},
    {"specular_texture", kSpirvFeatSpecular},
    {"fMaterialPower", kSpirvFeatSpecular},
    {"USE_SKINNING", kSpirvFeatSkinning},
    {"inBlendWeight", kSpirvFeatSkinning},
    {"matWorldArray", kSpirvFeatSkinning},
    {"discard", kSpirvFeatAlphaTest},
    {"alphatest", kSpirvFeatAlphaTest},
    {"instanced", kSpirvFeatInstancing},
    {"inColor0", kSpirvFeatVertexColor},
    {"VertexColor", kSpirvFeatVertexColor},
    {"env_texture", kSpirvFeatEnvMap},
    {"shadowmap_texture", kSpirvFeatShadowMap},
    {"output_gamma_inv", kSpirvFeatGammaOutput},
};

struct NameSlotEntry {
    std::string_view name;
    std::uint32_t slot;
};

inline constexpr NameSlotEntry kWarbandAttrLocations[] = {
    {"inPosition", 0U},
    {"inNormal", 1U},
    {"inTexCoord", 2U},
    {"inColor0", 3U},
    {"inColor1", 4U},
    {"inTangent", 5U},
    {"inBinormal", 6U},
    {"inBlendWeight", 7U},
    {"inBlendWeights", 7U},
    {"inBlendIndices", 8U},
    // Compatibility-profile attributes alias the Warband slot with the same meaning.
    {"_wse_gl_Vertex", 0U},
    {"_wse_gl_Normal", 1U},
    {"_wse_gl_MultiTexCoord0", 2U},
    {"_wse_gl_Color", 3U},
    {"_wse_gl_SecondaryColor", 4U},
};

// Graph-colored 9-location mapping covering all 39 Warband retail varying names
// with zero intra-technique conflicts. Names that collide inside one technique
// (possible in mod shaders) fall back to dynamic slots; see `assign_interface_layout`.
inline constexpr NameSlotEntry kWarbandVaryingLocations[] = {
    {"Fog", 0U},          {"TC", 0U},             {"Tex", 0U},         {"outTexCoord", 0U},
    {"texCoord0", 0U},    {"Tex0", 1U},           {"outColor0", 1U},   {"outDepth", 1U},
    {"texCoord1", 1U},    {"LightDir", 2U},       {"Map", 2U},         {"ViewDir", 2U},
    {"normal", 2U},       {"outColor1", 2U},      {"texCoord2", 2U},   {"vSpecular", 2U},
    {"Color", 3U},        {"LightDif", 3U},       {"VertexColor", 3U}, {"outFog", 3U},
    {"texCoord3", 3U},    {"CameraDir", 4U},      {"ShadowTexCoord", 4U},
    {"outNormal", 4U},    {"vec_to_light_0", 4U}, {"Depth", 5U},       {"VertexLighting", 5U},
    {"WorldNormal", 5U},  {"vec_to_light_1", 5U}, {"PosWater", 6U},    {"SkyLightDir", 6U},
    {"SunLight", 6U},     {"tangent", 6U},        {"vec_to_light_2", 6U},
    {"SunLightDir", 7U},  {"projCoord", 7U},      {"viewVec", 7U},     {"LightDir_Alpha", 8U},
    {"PointLightDir", 8U},
};

// Bindings 0 and 1 are reserved for the VS and FS `std140` uniform blocks.
inline constexpr NameSlotEntry kWarbandSamplerBindings[] = {
    {"depth_texture", 2U},
    {"diffuse_texture", 3U},
    {"diffuse_texture_2", 4U},
    {"env_texture", 5U},
    {"normal_texture", 6U},
    {"postFX_sampler0", 7U},
    {"postFX_sampler1", 8U},
    {"postFX_sampler2", 9U},
    {"postFX_sampler3", 10U},
    {"postFX_sampler4", 11U},
    {"shadowmap_texture", 12U},
    {"specular_texture", 13U},
};

// First binding / location handed to names outside the fixed tables.
inline constexpr std::uint32_t kFirstDynamicAttrLocation = 9U;
inline constexpr std::uint32_t kFirstDynamicVaryingLocation = 9U;
inline constexpr std::uint32_t kFirstDynamicSamplerBinding = 14U;
inline constexpr std::uint32_t kMaxInterfaceSlots = 64U;

[[nodiscard]] inline std::optional<std::uint32_t> lookup_name_slot(
    std::string_view name, std::span<const NameSlotEntry> table) noexcept {
    for (const NameSlotEntry& e : table) {
        if (e.name == name) {
            return e.slot;
        }
    }
    return std::nullopt;
}

// Heterogeneous (`std::string_view`) lookup so per-identifier probes do not allocate.
struct StringViewHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};
using StringSet = std::unordered_set<std::string, StringViewHash, std::equal_to<>>;
template <typename V>
using StringMap = std::unordered_map<std::string, V, StringViewHash, std::equal_to<>>;

// Joins `\`-newline continuations (GLSL 4.20+, and accepted by most GL drivers for
// older versions; glslang rejects them below 4.20). Lost newlines are re-emitted at
// the end of the joined logical line so diagnostics keep their line numbers.
[[nodiscard]] inline std::string splice_glsl_line_continuations(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    std::size_t pending_newlines = 0;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (c == '\\') {
            std::size_t j = i + 1U;
            if (j < src.size() && src[j] == '\r') {
                ++j;
            }
            if (j < src.size() && src[j] == '\n') {
                ++pending_newlines;
                i = j;
                continue;
            }
        }
        out.push_back(c);
        if (c == '\n') {
            out.append(pending_newlines, '\n');
            pending_newlines = 0;
        }
    }
    out.append(pending_newlines, '\n');
    return out;
}

// Replaces comments as the GLSL preprocessor does: each comment becomes one space
// (so `a/**/b` stays two tokens) and newlines inside block comments are kept.
[[nodiscard]] inline std::string strip_glsl_comments(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    std::size_t i = 0;
    while (i < src.size()) {
        const char c = src[i];
        if (c == '/' && i + 1U < src.size() && src[i + 1U] == '/') {
            i += 2U;
            while (i < src.size() && src[i] != '\n') {
                ++i;
            }
            out.push_back(' ');
        } else if (c == '/' && i + 1U < src.size() && src[i + 1U] == '*') {
            i += 2U;
            out.push_back(' ');
            while (i < src.size()) {
                if (src[i] == '*' && i + 1U < src.size() && src[i + 1U] == '/') {
                    i += 2U;
                    break;
                }
                if (src[i] == '\n') {
                    out.push_back('\n');
                }
                ++i;
            }
        } else {
            out.push_back(c);
            ++i;
        }
    }
    return out;
}

[[nodiscard]] inline bool is_glsl_ident_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

[[nodiscard]] inline bool is_glsl_ident_char(char c) noexcept {
    return is_glsl_ident_start(c) || (c >= '0' && c <= '9');
}

[[nodiscard]] inline bool is_glsl_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

[[nodiscard]] inline bool is_glsl_inline_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

// Length of the numeric literal starting at `s[i]` (`1`, `1.5e-3`, `.5`, `0x1F`, `2u`).
[[nodiscard]] inline std::size_t glsl_number_length(std::string_view s, std::size_t i) noexcept {
    const std::size_t start = i;
    const bool hex = (i + 1U < s.size() && s[i] == '0' && (s[i + 1U] == 'x' || s[i + 1U] == 'X'));
    while (i < s.size()) {
        const char c = s[i];
        if (is_glsl_ident_char(c) || c == '.') {
            ++i;
        } else if (!hex && (c == '+' || c == '-') && i > start &&
                   (s[i - 1U] == 'e' || s[i - 1U] == 'E')) {
            ++i;
        } else {
            break;
        }
    }
    return i - start;
}

// Directive name of a preprocessor line (`#version`, `# version`, `#if`, ...), or "".
[[nodiscard]] inline std::string_view glsl_directive_name(std::string_view line) noexcept {
    std::size_t i = 0;
    while (i < line.size() && is_glsl_inline_space(line[i])) {
        ++i;
    }
    if (i >= line.size() || line[i] != '#') {
        return {};
    }
    ++i;
    while (i < line.size() && is_glsl_inline_space(line[i])) {
        ++i;
    }
    const std::size_t b = i;
    while (i < line.size() && is_glsl_ident_char(line[i])) {
        ++i;
    }
    return line.substr(b, i - b);
}

[[nodiscard]] inline bool is_glsl_conditional_directive(std::string_view d) noexcept {
    return d == "if" || d == "ifdef" || d == "ifndef" || d == "elif" || d == "else" ||
           d == "endif";
}

// `matN`, `matNxM`, `dmatN`, `dmatNxM` (N, M in 2..4). Exact, so Warband uniforms such
// as `matWorld` or `matWorldViewProj` are never mistaken for types.
[[nodiscard]] inline bool is_glsl_matrix_type_name(std::string_view t) noexcept {
    if (t.starts_with("d")) {
        t.remove_prefix(1U);
    }
    if (!t.starts_with("mat")) {
        return false;
    }
    t.remove_prefix(3U);
    const auto dim = [](char c) { return c >= '2' && c <= '4'; };
    return (t.size() == 1U && dim(t[0])) || (t.size() == 3U && dim(t[0]) && t[1] == 'x' && dim(t[2]));
}

// Opaque sampler / image types (`sampler2D`, `usampler2D`, `sampler2DShadow`,
// `samplerCube`, `image2D`, ...): declared outside the uniform block with a binding.
[[nodiscard]] inline bool is_glsl_opaque_type_name(std::string_view t) noexcept {
    if (t.starts_with("i") && !t.starts_with("image")) {
        t.remove_prefix(1U);
    } else if (t.starts_with("u")) {
        t.remove_prefix(1U);
    }
    bool sampler = false;
    if (t.starts_with("sampler")) {
        t.remove_prefix(7U);
        sampler = true;
    } else if (t.starts_with("image")) {
        t.remove_prefix(5U);
    } else {
        return false;
    }
    if (sampler && t.ends_with("Shadow")) {
        t.remove_suffix(6U);
        if (t.empty()) {
            return true;  // `samplerShadow` (Vulkan GLSL)
        }
    }
    if (t.empty()) {
        return sampler;  // `sampler` (Vulkan GLSL)
    }
    constexpr std::string_view kDims[] = {"1D",       "2D",        "3D",      "Cube",
                                          "2DRect",   "1DArray",   "2DArray", "CubeArray",
                                          "Buffer",   "2DMS",      "2DMSArray"};
    for (const std::string_view d : kDims) {
        if (t == d) {
            return true;
        }
    }
    return false;
}

// Built-in GLSL type names a declaration may start with (precision and storage
// qualifiers are not types).
[[nodiscard]] inline bool is_glsl_builtin_type_name(std::string_view t) noexcept {
    if (t == "void" || t == "bool" || t == "int" || t == "uint" || t == "float" ||
        t == "double") {
        return true;
    }
    const auto has_dim_suffix = [&](std::string_view prefix) {
        return t.size() == prefix.size() + 1U && t.starts_with(prefix) && t.back() >= '2' &&
               t.back() <= '4';
    };
    if (has_dim_suffix("vec") || has_dim_suffix("ivec") || has_dim_suffix("uvec") ||
        has_dim_suffix("bvec") || has_dim_suffix("dvec")) {
        return true;
    }
    return is_glsl_matrix_type_name(t) || is_glsl_opaque_type_name(t);
}

// Interface locations used by one value of `type` (GLSL 4.50 section 4.4.1): a matrix
// takes one location per column, everything Warband declares otherwise takes one.
[[nodiscard]] inline std::uint32_t glsl_type_location_count(std::string_view type) noexcept {
    if (is_glsl_matrix_type_name(type)) {
        const std::size_t p = type.find_first_of("234");
        if (p != std::string_view::npos) {
            return static_cast<std::uint32_t>(type[p] - '0');
        }
    }
    return 1U;
}

// Product of the `[N]` dimensions in `suffix` (1 when empty), or nullopt when a
// dimension is not a positive decimal literal.
[[nodiscard]] inline std::optional<std::uint32_t> glsl_array_element_count(
    std::string_view suffix) noexcept {
    std::uint64_t total = 1U;
    std::size_t i = 0;
    while (i < suffix.size()) {
        if (suffix[i] != '[') {
            return std::nullopt;
        }
        ++i;
        std::uint64_t n = 0;
        std::size_t digits = 0;
        while (i < suffix.size() && is_glsl_digit(suffix[i])) {
            n = n * 10U + static_cast<std::uint64_t>(suffix[i] - '0');
            if (n > kMaxInterfaceSlots) {
                return std::nullopt;
            }
            ++i;
            ++digits;
        }
        if (digits == 0U || n == 0U || i >= suffix.size() || suffix[i] != ']') {
            return std::nullopt;
        }
        ++i;
        total *= n;
        if (total > kMaxInterfaceSlots) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(total);
}

[[nodiscard]] inline bool slot_range_free(std::uint64_t used_mask,
                                          std::uint32_t first,
                                          std::uint32_t count) noexcept {
    if (count == 0U || first >= kMaxInterfaceSlots || count > kMaxInterfaceSlots - first) {
        return false;
    }
    for (std::uint32_t s = first; s < first + count; ++s) {
        if ((used_mask & (1ULL << s)) != 0ULL) {
            return false;
        }
    }
    return true;
}

inline void mark_slot_range(std::uint64_t& used_mask, std::uint32_t first,
                            std::uint32_t count) noexcept {
    for (std::uint32_t s = first; s < first + count && s < kMaxInterfaceSlots; ++s) {
        used_mask |= (1ULL << s);
    }
}

// First free run of `count` slots at or above `min_slot`, then below it; nullopt when full.
[[nodiscard]] inline std::optional<std::uint32_t> alloc_slot_range(std::uint64_t& used_mask,
                                                                   std::uint32_t count,
                                                                   std::uint32_t min_slot,
                                                                   std::uint32_t floor_slot = 0U)
    noexcept {
    for (std::uint32_t s = min_slot; s < kMaxInterfaceSlots; ++s) {
        if (slot_range_free(used_mask, s, count)) {
            mark_slot_range(used_mask, s, count);
            return s;
        }
    }
    for (std::uint32_t s = floor_slot; s < min_slot; ++s) {
        if (slot_range_free(used_mask, s, count)) {
            mark_slot_range(used_mask, s, count);
            return s;
        }
    }
    return std::nullopt;
}

struct GlslDeclItem {
    std::string type;
    std::string name;
    std::string array_suffix;
    std::string interpolation;  // `flat` / `centroid` / `noperspective` / `smooth` or empty
    std::string initializer;    // uniform default value (GLSL text) or empty
    bool invariant = false;
};

// Result of splitting one Warband GLSL stage into legacy declarations and the rest.
struct ParsedWarbandGlsl {
    std::vector<GlslDeclItem> attributes;
    std::vector<GlslDeclItem> varyings;
    std::vector<GlslDeclItem> samplers;
    std::vector<GlslDeclItem> ubo_members;
    std::vector<GlslDeclItem> frag_outputs;  // GLSL 1.30+ fragment `out`, in source order
    std::string struct_defs;  // top-level `struct` definitions, emitted ahead of the UBO
    std::string preamble;     // `#extension` / `#pragma` / `#define` outside any `#if`
    std::string body;         // everything else, in source order
    std::string error;
    // Compatibility-profile built-ins replaced by synthesized declarations
    // (`gl_Vertex` -> `_wse_gl_Vertex`); see `synthesize_compat_builtins`.
    StringMap<std::string> builtin_renames;
    bool needs_ftransform = false;
};

// Parses one top-level `;`-terminated statement. Returns true when it was a legacy
// `uniform` / `attribute` / `varying` declaration (recorded into `p`, or `p.error`
// set); false leaves the statement to be copied through unchanged.
[[nodiscard]] inline bool parse_legacy_decl_statement(std::string_view stmt,
                                                      SpirvStage stage,
                                                      ParsedWarbandGlsl& p) {
    const std::string_view trimmed = trim_ascii(stmt);
    if (trimmed.empty() || trimmed.back() != ';') {
        return false;
    }
    std::vector<std::string_view> toks;
    std::size_t pos = 0;
    while (pos < trimmed.size()) {
        const char c = trimmed[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            ++pos;
            continue;
        }
        if (is_glsl_ident_start(c)) {
            const std::size_t s = pos++;
            while (pos < trimmed.size() && is_glsl_ident_char(trimmed[pos])) {
                ++pos;
            }
            toks.push_back(trimmed.substr(s, pos - s));
            continue;
        }
        if (is_glsl_digit(c) ||
            (c == '.' && pos + 1U < trimmed.size() && is_glsl_digit(trimmed[pos + 1U]))) {
            const std::size_t n = glsl_number_length(trimmed, pos);
            toks.push_back(trimmed.substr(pos, n));
            pos += n;
            continue;
        }
        toks.push_back(trimmed.substr(pos, 1U));
        ++pos;
    }

    std::size_t idx = 0;
    bool invariant = false;
    std::string_view interpolation;
    while (idx < toks.size()) {
        const std::string_view t = toks[idx];
        if (t == "invariant") {
            invariant = true;
        } else if (t == "centroid" || t == "flat" || t == "smooth" || t == "noperspective") {
            interpolation = t;
        } else {
            break;
        }
        ++idx;
    }
    if (idx >= toks.size()) {
        return false;
    }
    const std::string_view qual = toks[idx++];
    if (qual != "uniform" && qual != "attribute" && qual != "varying" && qual != "in" &&
        qual != "out") {
        return false;
    }
    // GLSL 1.30+ `in` / `out` at file scope: vertex `in` = attribute, `out` across the
    // stage boundary = varying, fragment `out` = color output.
    enum class Role : std::uint8_t { Uniform, Attribute, Varying, FragOutput };
    Role role = Role::Uniform;
    if (qual == "attribute" || (qual == "in" && stage == SpirvStage::Vertex)) {
        role = Role::Attribute;
    } else if (qual == "varying" || (qual == "out" && stage == SpirvStage::Vertex) ||
               (qual == "in" && stage == SpirvStage::Fragment)) {
        role = Role::Varying;
    } else if (qual == "out") {
        role = Role::FragOutput;
    }
    if ((invariant || !interpolation.empty()) && role != Role::Varying) {
        return false;
    }
    if (idx < toks.size() &&
        (toks[idx] == "highp" || toks[idx] == "mediump" || toks[idx] == "lowp")) {
        ++idx;
    }
    if (idx + 2U >= toks.size() || !is_glsl_ident_start(toks[idx].front())) {
        return false;
    }
    const std::string_view type = toks[idx++];

    const auto text_between = [&](std::size_t first_tok, std::size_t end_tok) {
        const std::string_view a = toks[first_tok];
        const std::string_view b = toks[end_tok - 1U];
        const std::size_t off = static_cast<std::size_t>(a.data() - trimmed.data());
        const std::size_t end = static_cast<std::size_t>(b.data() - trimmed.data()) + b.size();
        return std::string(trimmed.substr(off, end - off));
    };

    std::vector<GlslDeclItem> items;
    while (idx < toks.size()) {
        const std::string_view name = toks[idx];
        if (!is_glsl_ident_start(name.front())) {
            return false;
        }
        ++idx;
        GlslDeclItem item{};
        item.type = std::string(type);
        item.name = std::string(name);
        item.interpolation = std::string(interpolation);
        item.invariant = invariant;
        while (idx < toks.size() && toks[idx] == "[") {
            const std::size_t open = idx++;
            while (idx < toks.size() && toks[idx] != "]") {
                ++idx;
            }
            if (idx >= toks.size()) {
                return false;
            }
            ++idx;
            item.array_suffix += text_between(open, idx);
        }
        if (idx >= toks.size()) {
            return false;
        }
        if (toks[idx] == "=") {
            if (qual != "uniform" || is_glsl_opaque_type_name(type)) {
                return false;  // Not legal GLSL; let glslang report it in place.
            }
            const std::size_t init_begin = ++idx;
            int depth = 0;
            while (idx < toks.size()) {
                const std::string_view t = toks[idx];
                if (t == "(" || t == "[" || t == "{") {
                    ++depth;
                } else if (t == ")" || t == "]" || t == "}") {
                    --depth;
                } else if (depth == 0 && (t == "," || t == ";")) {
                    break;
                }
                ++idx;
            }
            if (idx >= toks.size() || idx == init_begin) {
                return false;
            }
            item.initializer = text_between(init_begin, idx);
        }
        items.push_back(std::move(item));
        if (toks[idx] == ";") {
            if (idx + 1U != toks.size()) {
                return false;
            }
            break;
        }
        if (toks[idx] != ",") {
            return false;
        }
        ++idx;
    }
    if (items.empty()) {
        return false;
    }

    for (GlslDeclItem& item : items) {
        if (role == Role::Attribute) {
            if (stage != SpirvStage::Vertex) {
                p.error = "attribute declaration not valid in fragment shader";
                return true;
            }
            p.attributes.push_back(std::move(item));
        } else if (role == Role::Varying) {
            p.varyings.push_back(std::move(item));
        } else if (role == Role::FragOutput) {
            p.frag_outputs.push_back(std::move(item));
        } else if (is_glsl_opaque_type_name(type)) {
            p.samplers.push_back(std::move(item));
        } else {
            p.ubo_members.push_back(std::move(item));
        }
    }
    return true;
}

// Pass 1: separate top-level preprocessor lines, legacy declarations, `struct`
// definitions, and the remaining code (kept in source order).
[[nodiscard]] inline ParsedWarbandGlsl parse_warband_glsl(std::string_view stripped,
                                                          SpirvStage stage) {
    ParsedWarbandGlsl p{};
    std::size_t i = 0;
    int brace_depth = 0;
    int pp_if_depth = 0;
    std::size_t stmt_start = 0;
    bool at_line_start = true;
    const auto stmt_is_struct = [&](std::size_t end) {
        const std::string_view s = trim_ascii(stripped.substr(stmt_start, end - stmt_start));
        return s.starts_with("struct") && (s.size() == 6U || !is_glsl_ident_char(s[6]));
    };
    while (i < stripped.size()) {
        const char c = stripped[i];
        if (brace_depth == 0 && at_line_start && is_glsl_inline_space(c)) {
            ++i;
            continue;
        }
        if (brace_depth == 0 && at_line_start && c == '#') {
            if (i > stmt_start) {
                p.body.append(stripped.substr(stmt_start, i - stmt_start));
            }
            const std::size_t eol = stripped.find('\n', i);
            const std::size_t next_i = (eol == std::string_view::npos) ? stripped.size() : (eol + 1U);
            std::string line(stripped.substr(i, next_i - i));
            if (line.empty() || line.back() != '\n') {
                line.push_back('\n');
            }
            const std::string_view directive = glsl_directive_name(line);
            if (directive == "version") {
                p.body.push_back('\n');  // Keep line numbering stable.
            } else if (is_glsl_conditional_directive(directive)) {
                // Conditionals stay in place so alternative definitions never coexist.
                if (directive == "if" || directive == "ifdef" || directive == "ifndef") {
                    ++pp_if_depth;
                } else if (directive == "endif" && pp_if_depth > 0) {
                    --pp_if_depth;
                }
                p.body.append(line);
            } else if (pp_if_depth == 0) {
                p.preamble.append(line);
                p.body.push_back('\n');
            } else {
                p.body.append(line);
            }
            i = next_i;
            stmt_start = i;
            at_line_start = true;
            continue;
        }
        at_line_start = (c == '\n');
        if (c == '{') {
            ++brace_depth;
        } else if (c == '}') {
            if (brace_depth == 0) {
                p.error = "unmatched '}' in GLSL source";
                return p;
            }
            --brace_depth;
            if (brace_depth == 0 && !stmt_is_struct(i + 1U)) {
                p.body.append(stripped.substr(stmt_start, i + 1U - stmt_start));
                stmt_start = i + 1U;
            }
        } else if (c == ';' && brace_depth == 0) {
            const std::string_view stmt = stripped.substr(stmt_start, i + 1U - stmt_start);
            if (stmt_is_struct(i + 1U)) {
                p.struct_defs.append(trim_ascii(stmt));
                p.struct_defs.push_back('\n');
            } else if (!parse_legacy_decl_statement(stmt, stage, p)) {
                p.body.append(stmt);
            }
            if (!p.error.empty()) {
                return p;
            }
            stmt_start = i + 1U;
        }
        ++i;
    }
    if (brace_depth != 0) {
        p.error = "unclosed '{' in GLSL source";
        return p;
    }
    if (stmt_start < stripped.size()) {
        p.body.append(stripped.substr(stmt_start));
    }
    return p;
}

// Locations / bindings shared by both stages of a technique.
struct WarbandInterfaceLayout {
    StringMap<std::uint32_t> attribute_locations;
    StringMap<std::uint32_t> varying_locations;
    StringMap<std::uint32_t> sampler_bindings;
    std::string error;
};

// Assigns varying locations and sampler bindings from the union of both stages'
// declarations, visiting names in sorted order, so lowering the vertex shader with the
// fragment shader as `partner` (and vice versa) yields identical interfaces. Fixed
// Warband slots win unless they would overlap an earlier name; every other name (and
// every array or matrix) gets the first free run of slots that fits it.
[[nodiscard]] inline WarbandInterfaceLayout assign_interface_layout(
    const ParsedWarbandGlsl& own, const ParsedWarbandGlsl* partner) {
    WarbandInterfaceLayout layout{};

    const auto assign = [&](const std::vector<const std::vector<GlslDeclItem>*>& lists,
                            std::span<const NameSlotEntry> table,
                            std::uint64_t used_mask,
                            std::uint32_t first_dynamic,
                            std::uint32_t floor_slot,
                            bool count_matrix_columns,
                            std::string_view what,
                            StringMap<std::uint32_t>& out) {
        std::map<std::string, std::uint32_t, std::less<>> sizes;
        for (const auto* list : lists) {
            for (const GlslDeclItem& d : *list) {
                const auto elems = glsl_array_element_count(d.array_suffix);
                if (!elems) {
                    layout.error = std::string(what) + " '" + d.name +
                                   "' needs an integer-literal array size";
                    return;
                }
                const std::uint32_t per =
                    count_matrix_columns ? glsl_type_location_count(d.type) : 1U;
                std::uint32_t& sz = sizes[d.name];
                sz = std::max(sz, *elems * per);
            }
        }
        std::vector<const std::string*> dynamic;
        for (const auto& [name, count] : sizes) {
            const auto fixed = lookup_name_slot(name, table);
            if (fixed && slot_range_free(used_mask, *fixed, count)) {
                mark_slot_range(used_mask, *fixed, count);
                out.emplace(name, *fixed);
            } else {
                dynamic.push_back(&name);
            }
        }
        for (const std::string* name : dynamic) {
            const auto slot = alloc_slot_range(used_mask, sizes[*name], first_dynamic, floor_slot);
            if (!slot) {
                layout.error = "out of " + std::string(what) + " slots for '" + *name + "'";
                return;
            }
            out.emplace(*name, *slot);
        }
    };

    std::vector<const std::vector<GlslDeclItem>*> varyings{&own.varyings};
    std::vector<const std::vector<GlslDeclItem>*> samplers{&own.samplers};
    if (partner != nullptr) {
        varyings.push_back(&partner->varyings);
        samplers.push_back(&partner->samplers);
    }
    assign(varyings, kWarbandVaryingLocations, 0ULL, kFirstDynamicVaryingLocation, 0U, true,
           "varying", layout.varying_locations);
    if (layout.error.empty()) {
        // Bindings 0 and 1 belong to the two stages' uniform blocks.
        assign(samplers, kWarbandSamplerBindings, 3ULL, kFirstDynamicSamplerBinding, 2U, false,
               "sampler", layout.sampler_bindings);
    }
    if (layout.error.empty()) {
        assign({&own.attributes}, kWarbandAttrLocations, 0ULL, kFirstDynamicAttrLocation, 0U,
               true, "attribute", layout.attribute_locations);
    }
    return layout;
}

// Texture built-ins removed from Vulkan GLSL and their `#version 450` spelling. The
// `shadow*` family returns `vec4` in GLSL 1.20 but `float` today, so it maps onto
// generated `_wse_shadow*` helpers that rebuild the old `(r, r, r, 1)` result.
struct GlslBuiltinRename {
    std::string_view legacy;
    std::string_view modern;
};

inline constexpr GlslBuiltinRename kLegacyTextureBuiltins[] = {
    {"texture1D", "texture"},
    {"texture1DProj", "textureProj"},
    {"texture1DLod", "textureLod"},
    {"texture1DProjLod", "textureProjLod"},
    {"texture2D", "texture"},
    {"texture2DProj", "textureProj"},
    {"texture2DLod", "textureLod"},
    {"texture2DProjLod", "textureProjLod"},
    {"texture2DLodARB", "textureLod"},
    {"texture2DProjLodARB", "textureProjLod"},
    {"texture2DGradARB", "textureGrad"},
    {"texture2DProjGradARB", "textureProjGrad"},
    {"texture3D", "texture"},
    {"texture3DProj", "textureProj"},
    {"texture3DLod", "textureLod"},
    {"texture3DProjLod", "textureProjLod"},
    {"textureCube", "texture"},
    {"textureCubeLod", "textureLod"},
    {"textureCubeLodARB", "textureLod"},
    {"textureCubeGradARB", "textureGrad"},
    {"shadow2D", "_wse_shadow2D"},
    {"shadow2DProj", "_wse_shadow2DProj"},
    {"shadow2DLod", "_wse_shadow2DLod"},
    {"shadow2DProjLod", "_wse_shadow2DProjLod"},
};

[[nodiscard]] inline std::string_view lookup_legacy_texture_builtin(std::string_view tok) noexcept {
    for (const GlslBuiltinRename& r : kLegacyTextureBuiltins) {
        if (r.legacy == tok) {
            return r.modern;
        }
    }
    return {};
}

// Index of the `gl_FragData[<literal>]` subscript that follows `pos`, or nullopt when
// the subscript is not a decimal literal.
[[nodiscard]] inline std::optional<std::uint32_t> frag_data_literal_index(std::string_view s,
                                                                          std::size_t pos) {
    while (pos < s.size() && (is_glsl_inline_space(s[pos]) || s[pos] == '\n')) {
        ++pos;
    }
    if (pos >= s.size() || s[pos] != '[') {
        return std::nullopt;
    }
    ++pos;
    while (pos < s.size() && is_glsl_inline_space(s[pos])) {
        ++pos;
    }
    std::uint32_t v = 0;
    std::size_t digits = 0;
    while (pos < s.size() && is_glsl_digit(s[pos]) && digits < 3U) {
        v = v * 10U + static_cast<std::uint32_t>(s[pos] - '0');
        ++pos;
        ++digits;
    }
    while (pos < s.size() && is_glsl_inline_space(s[pos])) {
        ++pos;
    }
    if (digits == 0U || pos >= s.size() || s[pos] != ']') {
        return std::nullopt;
    }
    return v;
}

// GLSL compatibility-profile built-ins (removed from Vulkan GLSL) that mod shaders use
// in place of Warband's own attributes and uniforms. Each becomes an ordinary legacy
// declaration, so it takes part in the shared interface layout and reflection:
// attributes alias the Warband slot of the same meaning (`gl_Vertex` -> location 0),
// matrices become uniform-block members that keep their `gl_` name in reflection, and
// `ftransform()` is `gl_ModelViewProjectionMatrix * gl_Vertex`. Fixed-function state
// structs (`gl_LightSource[]`, `gl_Fog`, `gl_FrontMaterial`, ...) and `gl_BackColor`
// are not provided and still fail in glslang.
struct CompatBuiltin {
    std::string_view name;
    std::string_view type;
    std::string_view replacement;  // empty: a uniform-block member keeping `name`
    enum Kind : std::uint8_t { VsAttribute, Varying, Uniform } kind;
};

inline constexpr CompatBuiltin kCompatVertexBuiltins[] = {
    {"gl_Vertex", "vec4", "_wse_gl_Vertex", CompatBuiltin::VsAttribute},
    {"gl_Normal", "vec3", "_wse_gl_Normal", CompatBuiltin::VsAttribute},
    {"gl_Color", "vec4", "_wse_gl_Color", CompatBuiltin::VsAttribute},
    {"gl_SecondaryColor", "vec4", "_wse_gl_SecondaryColor", CompatBuiltin::VsAttribute},
    {"gl_FogCoord", "float", "_wse_gl_FogCoord", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord0", "vec4", "_wse_gl_MultiTexCoord0", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord1", "vec4", "_wse_gl_MultiTexCoord1", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord2", "vec4", "_wse_gl_MultiTexCoord2", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord3", "vec4", "_wse_gl_MultiTexCoord3", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord4", "vec4", "_wse_gl_MultiTexCoord4", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord5", "vec4", "_wse_gl_MultiTexCoord5", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord6", "vec4", "_wse_gl_MultiTexCoord6", CompatBuiltin::VsAttribute},
    {"gl_MultiTexCoord7", "vec4", "_wse_gl_MultiTexCoord7", CompatBuiltin::VsAttribute},
    {"gl_FrontColor", "vec4", "_wse_gl_FrontColor", CompatBuiltin::Varying},
    {"gl_FrontSecondaryColor", "vec4", "_wse_gl_FrontSecondaryColor", CompatBuiltin::Varying},
};

inline constexpr CompatBuiltin kCompatFragmentBuiltins[] = {
    // In a fragment shader these read the interpolated front colors.
    {"gl_Color", "vec4", "_wse_gl_FrontColor", CompatBuiltin::Varying},
    {"gl_SecondaryColor", "vec4", "_wse_gl_FrontSecondaryColor", CompatBuiltin::Varying},
};

inline constexpr CompatBuiltin kCompatSharedBuiltins[] = {
    {"gl_FogFragCoord", "float", "_wse_gl_FogFragCoord", CompatBuiltin::Varying},
    {"gl_ModelViewMatrix", "mat4", "", CompatBuiltin::Uniform},
    {"gl_ProjectionMatrix", "mat4", "", CompatBuiltin::Uniform},
    {"gl_ModelViewProjectionMatrix", "mat4", "", CompatBuiltin::Uniform},
    {"gl_NormalMatrix", "mat3", "", CompatBuiltin::Uniform},
    {"gl_ModelViewMatrixInverse", "mat4", "", CompatBuiltin::Uniform},
    {"gl_ProjectionMatrixInverse", "mat4", "", CompatBuiltin::Uniform},
    {"gl_ModelViewProjectionMatrixInverse", "mat4", "", CompatBuiltin::Uniform},
    {"gl_ModelViewMatrixTranspose", "mat4", "", CompatBuiltin::Uniform},
    {"gl_ModelViewMatrixInverseTranspose", "mat4", "", CompatBuiltin::Uniform},
};

inline constexpr std::string_view kCompatTexCoordName = "_wse_gl_TexCoord";
inline constexpr std::uint32_t kCompatMaxTextureCoords = 8U;  // GL `gl_MaxTextureCoords`

// Adds the declarations for every compatibility built-in `p.body` uses (see
// `CompatBuiltin`). `gl_TexCoord[]` and `gl_TextureMatrix[]` are sized by the largest
// literal index used (`gl_MaxTextureCoords` = 8 for a computed index).
inline void synthesize_compat_builtins(ParsedWarbandGlsl& p, SpirvStage stage) {
    const bool vertex = (stage == SpirvStage::Vertex);
    StringSet used;
    std::uint32_t tex_coord_count = 0U;
    std::uint32_t tex_matrix_count = 0U;
    const std::string_view body = p.body;
    for (std::size_t i = 0; i < body.size();) {
        const char c = body[i];
        if (is_glsl_digit(c)) {
            i += std::max<std::size_t>(1U, glsl_number_length(body, i));
            continue;
        }
        if (!is_glsl_ident_start(c)) {
            ++i;
            continue;
        }
        const std::size_t b = i;
        while (i < body.size() && is_glsl_ident_char(body[i])) {
            ++i;
        }
        const std::string_view tok = body.substr(b, i - b);
        if (!tok.starts_with("gl_") && tok != "ftransform") {
            continue;
        }
        const auto count_for_index = [&](std::uint32_t& count) {
            const auto idx = frag_data_literal_index(body, i);
            count = std::max(count, idx ? std::min(*idx + 1U, kCompatMaxTextureCoords)
                                        : kCompatMaxTextureCoords);
        };
        if (tok == "gl_TexCoord") {
            count_for_index(tex_coord_count);
        } else if (tok == "gl_TextureMatrix") {
            count_for_index(tex_matrix_count);
        }
        used.emplace(tok);
    }
    if (used.empty()) {
        return;
    }
    if (vertex && used.contains("ftransform")) {
        p.needs_ftransform = true;
        p.builtin_renames.emplace("ftransform", "_wse_ftransform");
        used.emplace("gl_Vertex");
        used.emplace("gl_ModelViewProjectionMatrix");
    }
    const auto has_decl = [](const std::vector<GlslDeclItem>& list, std::string_view name) {
        return std::any_of(list.begin(), list.end(),
                           [&](const GlslDeclItem& d) { return d.name == name; });
    };
    const auto add = [&](const CompatBuiltin& bi) {
        if (!used.contains(bi.name) || p.builtin_renames.contains(bi.name)) {
            return;
        }
        GlslDeclItem d{};
        d.type = std::string(bi.type);
        if (bi.kind == CompatBuiltin::Uniform) {
            d.name = std::string(bi.name);
            if (!has_decl(p.ubo_members, d.name)) {
                p.ubo_members.push_back(std::move(d));
            }
            return;
        }
        d.name = std::string(bi.replacement);
        p.builtin_renames.emplace(std::string(bi.name), d.name);
        auto& list = (bi.kind == CompatBuiltin::VsAttribute) ? p.attributes : p.varyings;
        if (!has_decl(list, d.name)) {
            list.push_back(std::move(d));
        }
    };
    if (vertex) {
        for (const CompatBuiltin& bi : kCompatVertexBuiltins) {
            add(bi);
        }
    } else {
        for (const CompatBuiltin& bi : kCompatFragmentBuiltins) {
            add(bi);
        }
    }
    for (const CompatBuiltin& bi : kCompatSharedBuiltins) {
        add(bi);
    }
    if (tex_coord_count > 0U) {
        // A GLSL 1.20 redeclaration (`varying vec4 gl_TexCoord[3];`) sets the size.
        auto it = std::find_if(p.varyings.begin(), p.varyings.end(),
                               [](const GlslDeclItem& d) { return d.name == "gl_TexCoord"; });
        if (it != p.varyings.end()) {
            it->name = std::string(kCompatTexCoordName);
        } else {
            GlslDeclItem d{};
            d.type = "vec4";
            d.name = std::string(kCompatTexCoordName);
            d.array_suffix = "[" + std::to_string(tex_coord_count) + "]";
            p.varyings.push_back(std::move(d));
        }
        p.builtin_renames.emplace("gl_TexCoord", std::string(kCompatTexCoordName));
    }
    if (tex_matrix_count > 0U && !has_decl(p.ubo_members, "gl_TextureMatrix")) {
        GlslDeclItem d{};
        d.type = "mat4";
        d.name = "gl_TextureMatrix";
        d.array_suffix = "[" + std::to_string(tex_matrix_count) + "]";
        p.ubo_members.push_back(std::move(d));
    }
}

// Both stages must declare a shared synthesized varying (`_wse_gl_TexCoord[N]`) with
// the same array size, so the smaller declaration grows to the larger.
inline void unify_compat_varying_sizes(ParsedWarbandGlsl& a, ParsedWarbandGlsl& b) {
    for (GlslDeclItem& da : a.varyings) {
        if (!da.name.starts_with("_wse_gl_")) {
            continue;
        }
        for (GlslDeclItem& db : b.varyings) {
            if (db.name != da.name || db.array_suffix == da.array_suffix) {
                continue;
            }
            const auto na = glsl_array_element_count(da.array_suffix);
            const auto nb = glsl_array_element_count(db.array_suffix);
            if (na && nb) {
                (*na >= *nb ? db : da).array_suffix = (*na >= *nb ? da : db).array_suffix;
            }
        }
    }
}

}  // namespace shader_detail

// Feature flags from GLSL source plus the vertex declaration. Matching is by whole
// identifier (ASCII case-insensitive) outside comments; on preprocessor lines only
// `#define NAME` counts. Allocation-free.
[[nodiscard]] inline std::uint32_t detect_glsl_feature_flags(
    std::string_view glsl_source, VertexDeclType decl_type) noexcept {
    std::uint32_t flags = kSpirvFeatNone;
    if (decl_type == VertexDeclType::Bumped || decl_type == VertexDeclType::BumpedSkinned) {
        flags |= kSpirvFeatNormalMap;
    }
    if (decl_type == VertexDeclType::Skinned || decl_type == VertexDeclType::BumpedSkinned) {
        flags |= kSpirvFeatSkinning;
    }
    shader_detail::for_each_glsl_identifier(glsl_source, [&flags](std::string_view tok) noexcept {
        for (const shader_detail::GlslFeatureToken& ft : shader_detail::kGlslFeatureTokens) {
            if (shader_detail::equals_ci(tok, ft.token)) {
                flags |= ft.flag;
            }
        }
    });
    return flags;
}

// Port heuristic (no retail counterpart): the vertex declaration a technique's vertex
// shader expects, from the attributes it reads. Retail picks the declaration per mesh
// (`get_mesh_vertex_declaration_type`); tools without a mesh use this instead.
[[nodiscard]] inline VertexDeclType infer_vertex_decl_from_glsl(std::string_view vs_glsl) noexcept {
    bool bumped = false;
    bool skinned = false;
    shader_detail::for_each_glsl_identifier(vs_glsl, [&](std::string_view tok) noexcept {
        if (tok == "inTangent" || tok == "inBinormal") {
            bumped = true;
        } else if (tok == "inBlendIndices" || tok == "inBlendWeight" ||
                   tok == "inBlendWeights" || tok == "USE_SKINNING") {
            skinned = true;
        }
    });
    if (bumped && skinned) {
        return VertexDeclType::BumpedSkinned;
    }
    if (skinned) {
        return VertexDeclType::Skinned;
    }
    return bumped ? VertexDeclType::Bumped : VertexDeclType::Standard;
}

[[nodiscard]] inline std::optional<std::uint32_t> warband_vertex_attribute_location(
    std::string_view name) noexcept {
    return shader_detail::lookup_name_slot(name, shader_detail::kWarbandAttrLocations);
}

[[nodiscard]] inline std::optional<std::uint32_t> warband_varying_location(
    std::string_view name) noexcept {
    return shader_detail::lookup_name_slot(name, shader_detail::kWarbandVaryingLocations);
}

[[nodiscard]] inline std::optional<std::uint32_t> warband_sampler_binding(
    std::string_view name) noexcept {
    return shader_detail::lookup_name_slot(name, shader_detail::kWarbandSamplerBindings);
}

struct ShaderUniformMemberInfo {
    std::string name;
    std::uint32_t offset_bytes = 0;
    // Bytes the member occupies in the block: `array_stride_bytes * array_size` for
    // arrays, `matrix_stride_bytes * columns` for matrices, else the value size.
    std::uint32_t size_bytes = 0;
    std::uint32_t array_size = 0;  // 0 when the member is not an array
    std::uint32_t array_stride_bytes = 0;
    std::uint32_t matrix_stride_bytes = 0;
};

struct ShaderUniformBlockInfo {
    std::string name;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
    std::uint32_t size_bytes = 0;
    std::vector<ShaderUniformMemberInfo> members;
};

struct ShaderSamplerBindingInfo {
    std::string name;
    std::uint32_t set = 0;
    std::uint32_t binding = 0;
};

struct ShaderInputAttributeInfo {
    std::string name;
    std::uint32_t location = 0;
};

// A GLSL 1.20 `uniform T name = <initializer>;` default. Vulkan uniform blocks have no
// initializers, so the value must be written into the block by the uploader.
struct ShaderUniformDefault {
    std::string name;
    std::string initializer_glsl;
};

struct ShaderStageReflection {
    std::vector<ShaderUniformBlockInfo> uniform_blocks;
    std::vector<ShaderSamplerBindingInfo> samplers;
    std::vector<ShaderInputAttributeInfo> inputs;
    std::vector<ShaderUniformDefault> uniform_defaults;
};

struct SpirvCompileOutput {
    std::vector<std::uint32_t> spirv_words;
    ShaderStageReflection reflection;
    std::string lowered_glsl;
    std::uint32_t feature_flags = kSpirvFeatNone;
    std::string info_log;
};

class SpirvCompilerBackend {
public:
    virtual ~SpirvCompilerBackend() = default;
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;

    // Stored in each module's tag; a different value (e.g. a compiler upgrade) makes
    // `SpirvShaderCache` recompile instead of reusing the old module.
    [[nodiscard]] virtual std::string cache_identity() const { return backend_name(); }

    // Whether `preprocess_legacy_glsl` is implemented.
    [[nodiscard]] virtual bool supports_preprocessing() const noexcept { return false; }

    // Runs the GLSL preprocessor on legacy (OpenGL, `#version 110/120`) source so the
    // lowering sees resolved `#if` blocks and expanded macros.
    [[nodiscard]] virtual bool preprocess_legacy_glsl(std::string_view glsl_source,
                                                      SpirvStage stage,
                                                      std::string& out_source,
                                                      std::string* out_info_log) {
        (void)glsl_source;
        (void)stage;
        out_source.clear();
        if (out_info_log != nullptr) {
            *out_info_log = "preprocessing not supported by this backend";
        }
        return false;
    }

    // Must be safe to call from several threads at once.
    [[nodiscard]] virtual bool compile_vulkan_glsl_to_spirv(
        std::string_view vulkan_glsl_source,
        SpirvStage stage,
        std::string_view entry_point,
        std::vector<std::uint32_t>& out_spirv_words,
        ShaderStageReflection* out_reflection,
        std::string* out_info_log) = 0;
};

struct LoweredVulkanGlsl {
    bool ok = false;
    std::string glsl;
    std::string error;
    std::uint32_t uniform_block_binding = 0;
    std::vector<ShaderUniformDefault> uniform_defaults;
};

inline constexpr std::string_view kWseVertexUniformBlock = "WseVertexUniforms";
inline constexpr std::string_view kWseFragmentUniformBlock = "WseFragmentUniforms";
inline constexpr std::string_view kWseUniformMemberPrefix = "_wse_u_";

// Maps a reflected block member name (`WseFragmentUniforms._wse_u_vMaterialColor`,
// `_wse_u_light.d`) back to the Warband uniform name (`vMaterialColor`, `light.d`).
[[nodiscard]] inline std::string demangle_warband_uniform_member_name(std::string_view name) {
    for (const std::string_view block : {kWseVertexUniformBlock, kWseFragmentUniformBlock}) {
        if (name.size() > block.size() && name.starts_with(block) && name[block.size()] == '.') {
            name.remove_prefix(block.size() + 1U);
            break;
        }
    }
    if (name.starts_with(kWseUniformMemberPrefix)) {
        name.remove_prefix(kWseUniformMemberPrefix.size());
    }
    return std::string(name);
}

// Translates a Warband OpenGL `#version 110/120` shader (`uniform`, `attribute`,
// `varying`, `gl_FragColor`, `gl_FragData`, `texture2D`, `shadow2D`) into Vulkan
// `#version 450` GLSL.
//
// `partner_glsl` is the technique's other stage. Varying locations and sampler
// bindings are assigned from both stages' declarations, so the vertex and fragment
// shaders of a technique only agree when each is lowered with the other as partner.
// Without a partner, names outside the fixed Warband tables may land on different
// slots in the two stages.
//
// Expects preprocessed source for full fidelity (`compile_warband_glsl_to_spirv` runs
// the backend's preprocessor first). On raw source, `#if` blocks stay in place but
// declarations inside them are emitted unconditionally, and macro bodies are renamed
// without scope tracking.
[[nodiscard]] inline LoweredVulkanGlsl lower_warband_glsl_to_vulkan450(
    std::string_view glsl_source, SpirvStage stage, std::string_view partner_glsl = {}) {
    using namespace shader_detail;
    LoweredVulkanGlsl result{};
    result.uniform_block_binding = (stage == SpirvStage::Vertex) ? 0U : 1U;

    const std::string stripped = strip_glsl_comments(splice_glsl_line_continuations(glsl_source));
    if (trim_ascii(stripped).empty()) {
        result.error = "empty GLSL source";
        return result;
    }

    // Pass-through for source that is already Vulkan GLSL (`#version 4xx`, no legacy names).
    {
        std::size_t pos = 0;
        while (pos < stripped.size() &&
               (is_glsl_inline_space(stripped[pos]) || stripped[pos] == '\n')) {
            ++pos;
        }
        const std::size_t eol = stripped.find('\n', pos);
        const std::string_view first_line =
            std::string_view(stripped).substr(pos, eol == std::string::npos ? std::string::npos
                                                                            : eol - pos);
        if (glsl_directive_name(first_line) == "version") {
            const std::size_t d = first_line.find_first_of("0123456789");
            int version = 0;
            for (std::size_t k = d; k != std::string_view::npos && k < first_line.size() &&
                                    is_glsl_digit(first_line[k]);
                 ++k) {
                version = version * 10 + (first_line[k] - '0');
                if (version > 10000) {
                    break;
                }
            }
            bool legacy = false;
            for_each_glsl_identifier(stripped, [&legacy](std::string_view tok) noexcept {
                if (tok == "attribute" || tok == "varying" || tok == "gl_FragColor" ||
                    tok == "gl_FragData" || !lookup_legacy_texture_builtin(tok).empty()) {
                    legacy = true;
                }
            });
            if (version >= 400 && !legacy) {
                result.glsl = stripped;
                result.ok = true;
                return result;
            }
        }
    }

    ParsedWarbandGlsl parsed = parse_warband_glsl(stripped, stage);
    if (!parsed.error.empty()) {
        result.error = std::move(parsed.error);
        return result;
    }
    synthesize_compat_builtins(parsed, stage);
    std::optional<ParsedWarbandGlsl> partner;
    if (!trim_ascii(partner_glsl).empty()) {
        const SpirvStage other =
            (stage == SpirvStage::Vertex) ? SpirvStage::Fragment : SpirvStage::Vertex;
        partner = parse_warband_glsl(
            strip_glsl_comments(splice_glsl_line_continuations(partner_glsl)), other);
        if (!partner->error.empty()) {
            result.error = "partner stage: " + partner->error;
            return result;
        }
        synthesize_compat_builtins(*partner, other);
        unify_compat_varying_sizes(parsed, *partner);
    }
    const WarbandInterfaceLayout layout =
        assign_interface_layout(parsed, partner ? &*partner : nullptr);
    if (!layout.error.empty()) {
        result.error = layout.error;
        return result;
    }

    StringSet ubo_names;
    for (const GlslDeclItem& d : parsed.ubo_members) {
        ubo_names.insert(d.name);
    }
    StringSet struct_names;
    for_each_glsl_identifier(parsed.struct_defs, [&, prev = std::string_view{}](
                                                     std::string_view tok) mutable noexcept {
        if (prev == "struct") {
            struct_names.emplace(tok);
        }
        prev = tok;
    });

    const std::string_view ubo_inst =
        (stage == SpirvStage::Vertex) ? "_wse_vs_u" : "_wse_fs_u";
    bool uses_frag_color = false;
    bool uses_frag_data = false;
    std::uint32_t frag_data_count = 1U;
    bool saw_main = false;
    StringSet used_helpers;

    // Spelling of a global identifier in Vulkan GLSL ("" = unchanged).
    const auto global_rename = [&](std::string_view tok, std::string& out) -> bool {
        if (ubo_names.contains(tok)) {
            out.append(ubo_inst);
            out.push_back('.');
            out.append(kWseUniformMemberPrefix);
            out.append(tok);
            return true;
        }
        if (const auto it = parsed.builtin_renames.find(tok); it != parsed.builtin_renames.end()) {
            out.append(it->second);
            return true;
        }
        const std::string_view modern = lookup_legacy_texture_builtin(tok);
        if (!modern.empty()) {
            if (modern.starts_with("_wse_")) {
                used_helpers.emplace(modern);
            }
            out.append(modern);
            return true;
        }
        if (stage == SpirvStage::Fragment && tok == "gl_FragColor") {
            uses_frag_color = true;
            out.append("_wse_frag_color");
            return true;
        }
        return false;
    };

    // Renames identifiers in a `#define` replacement list (macro parameters excluded).
    const auto rewrite_define_line = [&](std::string_view line, std::string& out) {
        std::size_t i = line.find('#') + 1U;
        while (i < line.size() && is_glsl_inline_space(line[i])) ++i;
        while (i < line.size() && is_glsl_ident_char(line[i])) ++i;  // `define`
        while (i < line.size() && is_glsl_inline_space(line[i])) ++i;
        while (i < line.size() && is_glsl_ident_char(line[i])) ++i;  // macro name
        StringSet params;
        if (i < line.size() && line[i] == '(') {
            const std::size_t close = line.find(')', i);
            const std::size_t end = (close == std::string_view::npos) ? line.size() : close + 1U;
            for_each_glsl_identifier(line.substr(i, end - i),
                                     [&params](std::string_view t) noexcept { params.emplace(t); });
            i = end;
        }
        out.append(line.substr(0, i));
        while (i < line.size()) {
            const char c = line[i];
            if (is_glsl_ident_start(c)) {
                const std::size_t s = i;
                while (i < line.size() && is_glsl_ident_char(line[i])) ++i;
                const std::string_view tok = line.substr(s, i - s);
                const bool member = s > 0U && line[s - 1U] == '.';
                if (!member && !params.contains(tok) && tok == "gl_FragData" &&
                    stage == SpirvStage::Fragment) {
                    uses_frag_data = true;
                    const auto idx = frag_data_literal_index(line, i);
                    frag_data_count = std::max(frag_data_count, idx ? *idx + 1U : 4U);
                    out.append("_wse_frag_data");
                } else if (member || params.contains(tok) || !global_rename(tok, out)) {
                    out.append(tok);
                }
            } else if (is_glsl_digit(c)) {
                const std::size_t n = glsl_number_length(line, i);
                out.append(line.substr(i, n));
                i += n;
            } else {
                out.push_back(c);
                ++i;
            }
        }
    };

    // Pass 2: rewrite identifiers in the body, honoring locals, parameters, and struct
    // members that shadow a uniform's name.
    std::string rewritten_body;
    rewritten_body.reserve(parsed.body.size() + 256U);
    {
        const std::string_view body = parsed.body;
        std::vector<StringSet> scopes(1U);
        StringSet pending_params;
        int paren_depth = 0;
        int brace_depth = 0;
        bool prev_was_type = false;
        bool expect_struct_name = false;
        bool decl_active = false;
        int decl_paren_depth = 0;
        char prev_non_ws = '\0';
        bool at_line_start = true;
        // `for (int i = ...)` declares `i` in a scope that ends with the loop body (a
        // `{}` block or a single statement), not with the enclosing function.
        enum class ForPhase : std::uint8_t { Header, AwaitBody, BraceBody };
        struct ForScope {
            ForPhase phase;
            int brace_depth;   // depth of the `for` keyword
            int paren_depth;   // depth inside the header's `(`
        };
        std::vector<ForScope> for_scopes;
        bool for_keyword_pending = false;
        const auto close_finished_for_scopes = [&]() {
            while (!for_scopes.empty() && for_scopes.back().phase == ForPhase::AwaitBody &&
                   for_scopes.back().brace_depth == brace_depth &&
                   paren_depth < for_scopes.back().paren_depth) {
                for_scopes.pop_back();
                if (scopes.size() > 1U) {
                    scopes.pop_back();
                }
            }
        };
        const auto shadowed = [&](std::string_view tok) {
            if (paren_depth > 0 && brace_depth == 0 && pending_params.contains(tok)) {
                return true;
            }
            for (const StringSet& s : scopes) {
                if (s.contains(tok)) {
                    return true;
                }
            }
            return false;
        };
        std::size_t i = 0;
        while (i < body.size()) {
            const char c = body[i];
            if (at_line_start && is_glsl_inline_space(c)) {
                rewritten_body.push_back(c);
                ++i;
                continue;
            }
            if (at_line_start && c == '#') {
                const std::size_t eol = body.find('\n', i);
                const std::size_t next_i = (eol == std::string_view::npos) ? body.size() : (eol + 1U);
                const std::string_view line = body.substr(i, next_i - i);
                if (glsl_directive_name(line) == "define") {
                    rewrite_define_line(line, rewritten_body);
                } else {
                    rewritten_body.append(line);
                }
                i = next_i;
                at_line_start = true;
                prev_non_ws = '\n';
                prev_was_type = false;
                continue;
            }
            at_line_start = (c == '\n');
            if (is_glsl_ident_start(c)) {
                const std::size_t s = i++;
                while (i < body.size() && is_glsl_ident_char(body[i])) ++i;
                const std::string_view tok = body.substr(s, i - s);
                bool is_type = false;
                if (prev_non_ws == '.') {
                    rewritten_body.append(tok);  // Member access / swizzle.
                } else if (expect_struct_name) {
                    struct_names.emplace(tok);
                    expect_struct_name = false;
                    rewritten_body.append(tok);
                } else {
                    is_type = is_glsl_builtin_type_name(tok) || struct_names.contains(tok);
                    const bool declares =
                        !is_type && (prev_was_type || (decl_active && prev_non_ws == ',' &&
                                                       paren_depth == decl_paren_depth));
                    if (tok == "struct") {
                        expect_struct_name = true;
                        rewritten_body.append(tok);
                    } else if (tok == "for") {
                        for_keyword_pending = true;
                        rewritten_body.append(tok);
                    } else if (declares) {
                        if (paren_depth > 0 && brace_depth == 0) {
                            pending_params.emplace(tok);
                        } else {
                            scopes.back().emplace(tok);
                        }
                        decl_active = true;
                        decl_paren_depth = paren_depth;
                        if (tok == "main" && brace_depth == 0) {
                            saw_main = true;
                        }
                        rewritten_body.append(tok);
                    } else if (tok == "gl_FragData" && stage == SpirvStage::Fragment) {
                        uses_frag_data = true;
                        const auto idx = frag_data_literal_index(body, i);
                        frag_data_count = std::max(frag_data_count, idx ? *idx + 1U : 4U);
                        rewritten_body.append("_wse_frag_data");
                    } else if (shadowed(tok) || !global_rename(tok, rewritten_body)) {
                        rewritten_body.append(tok);
                    }
                }
                prev_was_type = is_type;
                prev_non_ws = 'A';
                continue;
            }
            if (is_glsl_digit(c) ||
                (c == '.' && i + 1U < body.size() && is_glsl_digit(body[i + 1U]))) {
                const std::size_t n = glsl_number_length(body, i);
                rewritten_body.append(body.substr(i, n));
                i += n;
                prev_was_type = false;
                prev_non_ws = '0';
                continue;
            }
            rewritten_body.push_back(c);
            ++i;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
                continue;
            }
            if (c == '[' || c == ']') {
                // `float k[4]` keeps the declaration open for a following `, j`.
                prev_non_ws = c;
                continue;
            }
            prev_was_type = false;
            if (c == '{') {
                if (!for_scopes.empty() && for_scopes.back().phase == ForPhase::AwaitBody &&
                    for_scopes.back().brace_depth == brace_depth) {
                    for_scopes.back().phase = ForPhase::BraceBody;
                }
                scopes.emplace_back();
                if (brace_depth == 0) {
                    scopes.back().swap(pending_params);
                    pending_params.clear();
                }
                ++brace_depth;
                decl_active = false;
            } else if (c == '}') {
                if (scopes.size() > 1U) {
                    scopes.pop_back();
                }
                if (brace_depth > 0) {
                    --brace_depth;
                }
                decl_active = false;
                if (!for_scopes.empty() && for_scopes.back().phase == ForPhase::BraceBody &&
                    for_scopes.back().brace_depth == brace_depth) {
                    // The loop body closed; any enclosing single-statement `for` ends too.
                    for_scopes.back().phase = ForPhase::AwaitBody;
                    close_finished_for_scopes();
                }
            } else if (c == '(') {
                ++paren_depth;
                if (for_keyword_pending && brace_depth > 0) {
                    scopes.emplace_back();
                    for_scopes.push_back({ForPhase::Header, brace_depth, paren_depth});
                }
                for_keyword_pending = false;
            } else if (c == ')') {
                --paren_depth;
                if (paren_depth < decl_paren_depth) {
                    decl_active = false;
                }
                if (!for_scopes.empty() && for_scopes.back().phase == ForPhase::Header &&
                    paren_depth < for_scopes.back().paren_depth) {
                    for_scopes.back().phase = ForPhase::AwaitBody;
                }
            } else if (c == ';') {
                decl_active = false;
                if (brace_depth == 0) {
                    pending_params.clear();
                }
                close_finished_for_scopes();
            }
            prev_non_ws = c;
        }
    }

    if (!saw_main) {
        result.error = "GLSL source missing main() entry point";
        return result;
    }
    if (uses_frag_color && uses_frag_data) {
        result.error = "fragment shader writes both gl_FragColor and gl_FragData";
        return result;
    }
    if (!parsed.frag_outputs.empty() && (uses_frag_color || uses_frag_data)) {
        result.error = "fragment shader mixes `out` variables with gl_FragColor / gl_FragData";
        return result;
    }

    const auto append_decl = [](std::string& out, std::string_view head, const GlslDeclItem& d) {
        out.append(head);
        out.append(d.type);
        out.push_back(' ');
        out.append(d.name);
        out.append(d.array_suffix);
        out.append(";\n");
    };

    std::string out;
    out.reserve(rewritten_body.size() + 2048U);
    out.append("#version 450\n");
    {
        // Preamble `#define`s get the same renaming as the body.
        const std::string_view pre = parsed.preamble;
        std::size_t s = 0;
        while (s < pre.size()) {
            const std::size_t e = pre.find('\n', s);
            const std::size_t n = (e == std::string_view::npos) ? pre.size() : e + 1U;
            const std::string_view line = pre.substr(s, n - s);
            if (glsl_directive_name(line) == "define") {
                rewrite_define_line(line, out);
            } else {
                out.append(line);
            }
            s = n;
        }
    }
    out.append(parsed.struct_defs);
    for (const GlslDeclItem& d : parsed.attributes) {
        append_decl(out,
                    "layout(location = " + std::to_string(layout.attribute_locations.at(d.name)) +
                        ") in ",
                    d);
    }
    const bool vertex = (stage == SpirvStage::Vertex);
    for (const GlslDeclItem& d : parsed.varyings) {
        std::string head = "layout(location = " + std::to_string(layout.varying_locations.at(d.name)) + ") ";
        if (d.invariant && vertex) {
            head += "invariant ";
        }
        if (!d.interpolation.empty()) {
            head += d.interpolation;
            head.push_back(' ');
        }
        head += vertex ? "out " : "in ";
        append_decl(out, head, d);
    }
    if (!vertex && uses_frag_color) {
        out.append("layout(location = 0) out vec4 _wse_frag_color;\n");
    }
    {
        // Color outputs take consecutive locations in declaration order (what a GL
        // driver assigns without `glBindFragDataLocation`).
        std::uint32_t next_location = 0U;
        for (const GlslDeclItem& d : parsed.frag_outputs) {
            append_decl(out, "layout(location = " + std::to_string(next_location) + ") out ", d);
            next_location += glsl_array_element_count(d.array_suffix).value_or(1U);
        }
    }
    if (!vertex && uses_frag_data) {
        out.append("layout(location = 0) out vec4 _wse_frag_data[");
        out.append(std::to_string(frag_data_count));
        out.append("];\n");
    }
    for (const GlslDeclItem& d : parsed.samplers) {
        append_decl(out,
                    "layout(set = 0, binding = " +
                        std::to_string(layout.sampler_bindings.at(d.name)) + ") uniform ",
                    d);
    }
    if (!parsed.ubo_members.empty()) {
        out.append("layout(set = 0, binding = ");
        out.append(std::to_string(result.uniform_block_binding));
        out.append(", std140) uniform ");
        out.append(vertex ? kWseVertexUniformBlock : kWseFragmentUniformBlock);
        out.append(" {\n");
        for (const GlslDeclItem& d : parsed.ubo_members) {
            out.append("    ");
            out.append(d.type);
            out.push_back(' ');
            out.append(kWseUniformMemberPrefix);
            out.append(d.name);
            out.append(d.array_suffix);
            out.append(";\n");
            if (!d.initializer.empty()) {
                result.uniform_defaults.push_back({d.name, d.initializer});
            }
        }
        out.append("} ");
        out.append(ubo_inst);
        out.append(";\n");
    }
    if (parsed.needs_ftransform) {
        out.append("vec4 _wse_ftransform() { return _wse_vs_u.");
        out.append(kWseUniformMemberPrefix);
        out.append("gl_ModelViewProjectionMatrix * _wse_gl_Vertex; }\n");
    }
    // `vec4(r, r, r, 1)`: GLSL 1.20 shadow lookups with the default luminance depth mode.
    if (used_helpers.contains("_wse_shadow2D")) {
        out.append("vec4 _wse_shadow2D(sampler2DShadow s, vec3 c) "
                   "{ return vec4(vec3(texture(s, c)), 1.0); }\n");
        if (!vertex) {
            out.append("vec4 _wse_shadow2D(sampler2DShadow s, vec3 c, float b) "
                       "{ return vec4(vec3(texture(s, c, b)), 1.0); }\n");
        }
    }
    if (used_helpers.contains("_wse_shadow2DProj")) {
        out.append("vec4 _wse_shadow2DProj(sampler2DShadow s, vec4 c) "
                   "{ return vec4(vec3(textureProj(s, c)), 1.0); }\n");
        if (!vertex) {
            out.append("vec4 _wse_shadow2DProj(sampler2DShadow s, vec4 c, float b) "
                       "{ return vec4(vec3(textureProj(s, c, b)), 1.0); }\n");
        }
    }
    if (used_helpers.contains("_wse_shadow2DLod")) {
        out.append("vec4 _wse_shadow2DLod(sampler2DShadow s, vec3 c, float l) "
                   "{ return vec4(vec3(textureLod(s, c, l)), 1.0); }\n");
    }
    if (used_helpers.contains("_wse_shadow2DProjLod")) {
        out.append("vec4 _wse_shadow2DProjLod(sampler2DShadow s, vec4 c, float l) "
                   "{ return vec4(vec3(textureProjLod(s, c, l)), 1.0); }\n");
    }
    out.append(rewritten_body);
    result.glsl = std::move(out);
    result.ok = true;
    return result;
}

// Injects a metadata `OpString` into a SPIR-V 1.0 module in Section 7(a) debug order
// (immediately after Capabilities / Extensions / ExtInstImports / MemoryModel /
// EntryPoints / ExecutionModes / earlier OpStrings) and advances the ID bound.
[[nodiscard]] inline bool inject_spirv_tag_opstring(std::vector<std::uint32_t>& words,
                                                    std::string_view tag) {
    if (words.size() < 6U || words[0] != 0x07230203U || words[3] < 2U) {
        return false;
    }
    std::size_t insert_idx = 5U;
    std::size_t idx = 5U;
    while (idx < words.size()) {
        const std::uint32_t wc = words[idx] >> 16U;
        const std::uint32_t op = words[idx] & 0xFFFFU;
        if (wc == 0U || idx + wc > words.size()) {
            return false;
        }
        if (op == 17U || op == 10U || op == 11U || op == 14U || op == 15U || op == 16U ||
            op == 331U || op == 7U) {
            insert_idx = idx + wc;
            idx += wc;
            continue;
        }
        break;
    }
    const std::uint32_t new_id = words[3]++;
    std::vector<std::uint32_t> inst;
    inst.push_back(0U);
    inst.push_back(new_id);
    shader_detail::append_spirv_string(inst, tag);
    inst[0] = (static_cast<std::uint32_t>(inst.size()) << 16U) | 7U;
    words.insert(words.begin() + static_cast<std::ptrdiff_t>(insert_idx), inst.begin(), inst.end());
    return true;
}

struct SpirvModuleInfo {
    std::uint32_t execution_model = 0;
    std::uint32_t decl_type = 0;
    std::uint32_t feature_flags = 0;
    std::uint64_t source_hash = 0;
    std::uint32_t generator_version = 0;
    std::string entry_name;
    std::string backend_identity;  // empty for tags written without `:be=`
};

// Validates a SPIR-V 1.0 module (header, non-placeholder generator, instruction bounds,
// MemoryModel, EntryPoint, trailing `OpFunctionEnd`) and decodes the `wse2:` `OpString` tag.
[[nodiscard]] inline std::optional<SpirvModuleInfo> inspect_spirv_module(
    std::span<const std::uint32_t> words) {
    using namespace shader_detail;
    if (words.size() < 6U || words[0] != 0x07230203U || words[1] != 0x00010000U ||
        words[2] == kLegacyPlaceholderSpirvMagic || words[3] < 2U || words[4] != 0U) {
        return std::nullopt;
    }
    SpirvModuleInfo info{};
    bool have_memory_model = false;
    bool have_entry = false;
    bool have_function = false;
    std::optional<std::string> tag;
    std::uint32_t last_opcode = 0;
    std::size_t idx = 5U;
    while (idx < words.size()) {
        const std::uint32_t wc = words[idx] >> 16U;
        const std::uint32_t opcode = words[idx] & 0xFFFFU;
        if (wc == 0U || idx + wc > words.size()) {
            return std::nullopt;
        }
        if (opcode == 14U) {
            have_memory_model = true;
        } else if (opcode == 15U && wc >= 4U) {
            info.execution_model = words[idx + 1U];
            if (words[idx + 2U] == 0U || words[idx + 2U] >= words[3]) {
                return std::nullopt;
            }
            const auto ep = read_spirv_string(words.subspan(idx + 3U, wc - 3U));
            if (!ep || ep->empty()) {
                return std::nullopt;
            }
            info.entry_name = *ep;
            have_entry = true;
        } else if (opcode == 7U && wc >= 3U) {
            if (words[idx + 1U] == 0U || words[idx + 1U] >= words[3]) {
                return std::nullopt;
            }
            const auto s = read_spirv_string(words.subspan(idx + 2U, wc - 2U));
            if (!s) {
                return std::nullopt;
            }
            if (s->starts_with("wse2:")) {
                tag = *s;
            }
        } else if (opcode == 54U) {
            have_function = true;
        }
        last_opcode = opcode;
        idx += wc;
    }
    if (!have_memory_model || !have_entry || !have_function || !tag || last_opcode != 56U) {
        return std::nullopt;
    }
    const std::string_view t(*tag);
    const std::size_t decl_at = t.rfind(":decl=");
    const std::size_t feat_at = t.rfind(":feat=");
    const std::size_t src_at = t.rfind(":src=");
    const std::size_t gen_at = t.rfind(":gen=");
    if (decl_at == std::string_view::npos || feat_at == std::string_view::npos ||
        src_at == std::string_view::npos || gen_at == std::string_view::npos ||
        !(decl_at < feat_at && feat_at < src_at && src_at < gen_at)) {
        return std::nullopt;
    }
    const std::string_view decl_text = t.substr(decl_at + 6U, feat_at - (decl_at + 6U));
    const auto feat = parse_hex_u64(t.substr(feat_at + 6U, src_at - (feat_at + 6U)));
    const auto src = parse_hex_u64(t.substr(src_at + 5U, gen_at - (src_at + 5U)));
    const std::string_view gen_text = t.substr(gen_at + 5U);
    if (decl_text.empty() || decl_text.size() > 4U ||
        !std::all_of(decl_text.begin(), decl_text.end(),
                     [](char c) { return c >= '0' && c <= '9'; }) ||
        !feat || !src || gen_text.empty() || gen_text.size() > 9U ||
        !std::all_of(gen_text.begin(), gen_text.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
        return std::nullopt;
    }
    const std::size_t be_at = t.substr(0, decl_at).rfind(":be=");
    if (be_at != std::string_view::npos) {
        info.backend_identity = std::string(t.substr(be_at + 4U, decl_at - (be_at + 4U)));
    }
    info.decl_type = static_cast<std::uint32_t>(std::stoul(std::string(decl_text)));
    info.feature_flags = static_cast<std::uint32_t>(*feat);
    info.source_hash = *src;
    info.generator_version = static_cast<std::uint32_t>(std::stoul(std::string(gen_text)));
    return info;
}

// Hash identifying a stage's source together with its partner stage (both shape the
// lowered interface). Equals `fnv1a64(glsl_source)` when there is no partner.
[[nodiscard]] inline std::uint64_t spirv_source_key(std::string_view glsl_source,
                                                    std::string_view partner_glsl = {}) noexcept {
    std::uint64_t h = shader_detail::fnv1a64(glsl_source);
    if (!partner_glsl.empty()) {
        h = shader_detail::fnv1a64(partner_glsl, shader_detail::fnv1a64("\x1fpartner\x1f", h));
    }
    return h;
}

[[nodiscard]] inline std::string sanitize_spirv_tag_field(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c == ':' || c == '\0') ? '_' : c);
    }
    return out;
}

// Preprocesses (when the backend can), lowers, and compiles one stage. Pass the
// technique's other stage as `partner_glsl` so both stages share one interface.
[[nodiscard]] inline bool compile_warband_glsl_to_spirv(
    SpirvCompilerBackend& backend,
    std::string_view glsl_source,
    SpirvStage stage,
    std::string_view technique_name,
    VertexDeclType decl_type,
    SpirvCompileOutput& out,
    std::string_view partner_glsl = {}) {
    using namespace shader_detail;
    out = {};
    out.feature_flags = detect_glsl_feature_flags(glsl_source, decl_type);

    std::string pre_source;
    std::string pre_partner;
    std::string_view lower_source = glsl_source;
    std::string_view lower_partner = partner_glsl;
    if (backend.supports_preprocessing()) {
        if (!backend.preprocess_legacy_glsl(splice_glsl_line_continuations(glsl_source), stage,
                                            pre_source, &out.info_log)) {
            if (out.info_log.empty()) {
                out.info_log = "GLSL preprocessing failed";
            }
            return false;
        }
        lower_source = pre_source;
        if (!trim_ascii(partner_glsl).empty()) {
            const SpirvStage other =
                (stage == SpirvStage::Vertex) ? SpirvStage::Fragment : SpirvStage::Vertex;
            std::string partner_log;
            if (!backend.preprocess_legacy_glsl(splice_glsl_line_continuations(partner_glsl),
                                                other, pre_partner, &partner_log)) {
                out.info_log = "partner stage preprocessing failed: " + partner_log;
                return false;
            }
            lower_partner = pre_partner;
        }
    }

    LoweredVulkanGlsl lowered = lower_warband_glsl_to_vulkan450(lower_source, stage, lower_partner);
    if (!lowered.ok) {
        out.info_log = std::move(lowered.error);
        return false;
    }
    out.lowered_glsl = std::move(lowered.glsl);

    std::vector<std::uint32_t> spv;
    if (!backend.compile_vulkan_glsl_to_spirv(out.lowered_glsl, stage, "main", spv,
                                              &out.reflection, &out.info_log)) {
        if (out.info_log.empty()) {
            out.info_log = "SPIR-V compilation failed";
        }
        return false;
    }
    for (ShaderUniformBlockInfo& block : out.reflection.uniform_blocks) {
        for (ShaderUniformMemberInfo& m : block.members) {
            m.name = demangle_warband_uniform_member_name(m.name);
        }
    }
    out.reflection.uniform_defaults = std::move(lowered.uniform_defaults);

    const std::string tag = "wse2:tech=" + sanitize_spirv_tag_field(technique_name) +
                            ":be=" + sanitize_spirv_tag_field(backend.cache_identity()) +
                            ":decl=" + std::to_string(static_cast<int>(decl_type)) +
                            ":feat=" + hex_u64(out.feature_flags) +
                            ":src=" + hex_u64(spirv_source_key(glsl_source, partner_glsl)) +
                            ":gen=" + std::to_string(kWseSpirvGeneratorVersion);
    if (!inject_spirv_tag_opstring(spv, tag) || !inspect_spirv_module(spv)) {
        out.info_log = "emitted SPIR-V failed post-tag validation";
        return false;
    }
    out.spirv_words = std::move(spv);
    return true;
}

namespace shader_detail {

inline constexpr std::uint32_t kReflectionSidecarMagic = 0x52455357U;  // "WSER"

inline void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
    }
}

inline void put_u64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    put_u32(b, static_cast<std::uint32_t>(v));
    put_u32(b, static_cast<std::uint32_t>(v >> 32U));
}

inline void put_str(std::vector<std::uint8_t>& b, std::string_view s) {
    put_u32(b, static_cast<std::uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

struct ByteReader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos = 0;
    bool ok = true;

    std::uint32_t u32() {
        if (!ok || bytes.size() - pos < 4U) {
            ok = false;
            return 0;
        }
        std::uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            v |= static_cast<std::uint32_t>(bytes[pos++]) << (8 * k);
        }
        return v;
    }
    std::uint64_t u64() {
        const std::uint64_t lo = u32();
        return lo | (static_cast<std::uint64_t>(u32()) << 32U);
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!ok || bytes.size() - pos < n) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(bytes.data() + pos), n);
        pos += n;
        return s;
    }
    // Element count bounded by the bytes left, so corrupt input cannot over-allocate.
    std::uint32_t count(std::size_t min_element_bytes) {
        const std::uint32_t n = u32();
        if (!ok || n > (bytes.size() - pos) / std::max<std::size_t>(1U, min_element_bytes)) {
            ok = false;
            return 0;
        }
        return n;
    }
};

}  // namespace shader_detail

// `<hash>.refl` sidecar next to each cached `.spv`, so cache hits return the same
// reflection a fresh compile does. Bound to the module by generator version and key.
[[nodiscard]] inline std::vector<std::uint8_t> serialize_shader_reflection(
    const ShaderStageReflection& r, std::uint64_t source_key) {
    using namespace shader_detail;
    std::vector<std::uint8_t> b;
    put_u32(b, kReflectionSidecarMagic);
    put_u32(b, kWseSpirvGeneratorVersion);
    put_u64(b, source_key);
    put_u32(b, static_cast<std::uint32_t>(r.uniform_blocks.size()));
    for (const ShaderUniformBlockInfo& blk : r.uniform_blocks) {
        put_str(b, blk.name);
        put_u32(b, blk.set);
        put_u32(b, blk.binding);
        put_u32(b, blk.size_bytes);
        put_u32(b, static_cast<std::uint32_t>(blk.members.size()));
        for (const ShaderUniformMemberInfo& m : blk.members) {
            put_str(b, m.name);
            put_u32(b, m.offset_bytes);
            put_u32(b, m.size_bytes);
            put_u32(b, m.array_size);
            put_u32(b, m.array_stride_bytes);
            put_u32(b, m.matrix_stride_bytes);
        }
    }
    put_u32(b, static_cast<std::uint32_t>(r.samplers.size()));
    for (const ShaderSamplerBindingInfo& s : r.samplers) {
        put_str(b, s.name);
        put_u32(b, s.set);
        put_u32(b, s.binding);
    }
    put_u32(b, static_cast<std::uint32_t>(r.inputs.size()));
    for (const ShaderInputAttributeInfo& in : r.inputs) {
        put_str(b, in.name);
        put_u32(b, in.location);
    }
    put_u32(b, static_cast<std::uint32_t>(r.uniform_defaults.size()));
    for (const ShaderUniformDefault& d : r.uniform_defaults) {
        put_str(b, d.name);
        put_str(b, d.initializer_glsl);
    }
    return b;
}

[[nodiscard]] inline std::optional<ShaderStageReflection> deserialize_shader_reflection(
    std::span<const std::uint8_t> bytes, std::uint64_t source_key) {
    shader_detail::ByteReader rd{bytes};
    if (rd.u32() != shader_detail::kReflectionSidecarMagic ||
        rd.u32() != kWseSpirvGeneratorVersion || rd.u64() != source_key || !rd.ok) {
        return std::nullopt;
    }
    ShaderStageReflection r{};
    r.uniform_blocks.resize(rd.count(20U));
    for (ShaderUniformBlockInfo& blk : r.uniform_blocks) {
        blk.name = rd.str();
        blk.set = rd.u32();
        blk.binding = rd.u32();
        blk.size_bytes = rd.u32();
        blk.members.resize(rd.count(24U));
        for (ShaderUniformMemberInfo& m : blk.members) {
            m.name = rd.str();
            m.offset_bytes = rd.u32();
            m.size_bytes = rd.u32();
            m.array_size = rd.u32();
            m.array_stride_bytes = rd.u32();
            m.matrix_stride_bytes = rd.u32();
        }
    }
    r.samplers.resize(rd.count(12U));
    for (ShaderSamplerBindingInfo& s : r.samplers) {
        s.name = rd.str();
        s.set = rd.u32();
        s.binding = rd.u32();
    }
    r.inputs.resize(rd.count(8U));
    for (ShaderInputAttributeInfo& in : r.inputs) {
        in.name = rd.str();
        in.location = rd.u32();
    }
    r.uniform_defaults.resize(rd.count(8U));
    for (ShaderUniformDefault& d : r.uniform_defaults) {
        d.name = rd.str();
        d.initializer_glsl = rd.str();
    }
    if (!rd.ok || rd.pos != bytes.size()) {
        return std::nullopt;
    }
    return r;
}

class SpirvShaderCache {
public:
    std::shared_ptr<SpirvCompilerBackend> compiler_backend;
    std::size_t cache_hits = 0;
    std::size_t compile_count = 0;
    std::string last_error;

    explicit SpirvShaderCache(std::shared_ptr<SpirvCompilerBackend> backend = nullptr)
        : compiler_backend(std::move(backend)) {}

    // Keyed by content only (stage, vertex declaration, this stage's source, partner
    // stage source), so techniques sharing GLSL files share one compiled module.
    [[nodiscard]] static std::string compute_cache_filename(SpirvStage stage,
                                                            VertexDeclType decl_type,
                                                            std::string_view glsl_source,
                                                            std::string_view partner_glsl = {}) {
        std::uint64_t h = shader_detail::fnv1a64(
            "wse2_spv_gen" + std::to_string(kWseSpirvGeneratorVersion) + ":");
        h = shader_detail::fnv1a64(stage == SpirvStage::Vertex ? ":vs:" : ":fs:", h);
        h = shader_detail::fnv1a64(std::to_string(static_cast<int>(decl_type)), h);
        h = shader_detail::fnv1a64(":", h);
        const std::uint64_t key = spirv_source_key(glsl_source, partner_glsl);
        for (int k = 0; k < 8; ++k) {
            h = (h ^ ((key >> (8 * k)) & 0xFFU)) * 1099511628211ULL;
        }
        return shader_detail::hex_u64(h) + ".spv";
    }

    // Returns the cached SPIR-V 1.0 module when it validates completely and its `OpString`
    // tag matches this source (+ partner), stage, vertex declaration, generator version,
    // and (when a backend is set) backend identity; otherwise lowers & compiles through
    // `compiler_backend` and persists `<hash>.spv` + `<hash>.refl` via `.partial` +
    // `rename_file`. `out_reflection` is filled on hits and misses alike.
    [[nodiscard]] bool get_or_compile(wse::platform::Vfs& cache_vfs,
                                      std::string_view cache_dir,
                                      std::string_view technique_name,
                                      SpirvStage stage,
                                      VertexDeclType decl_type,
                                      std::string_view glsl_source,
                                      std::vector<std::uint32_t>& out_words,
                                      bool* out_cache_hit = nullptr,
                                      std::uint32_t* out_feature_flags = nullptr,
                                      ShaderStageReflection* out_reflection = nullptr,
                                      std::string_view partner_glsl = {}) {
        out_words.clear();
        last_error.clear();
        if (out_cache_hit != nullptr) {
            *out_cache_hit = false;
        }
        const std::uint64_t key = spirv_source_key(glsl_source, partner_glsl);
        const std::string fname = compute_cache_filename(stage, decl_type, glsl_source, partner_glsl);
        const std::string path = wse::game::vfs_join_path(cache_dir, fname);
        const std::string refl_path = path.substr(0, path.size() - 4U) + ".refl";

        std::vector<std::uint8_t> raw;
        if (cache_vfs.read_file(path, raw) && raw.size() >= 24U && (raw.size() % 4U) == 0U) {
            std::vector<std::uint32_t> cached(raw.size() / 4U);
            std::memcpy(cached.data(), raw.data(), raw.size());
            const std::optional<SpirvModuleInfo> info = inspect_spirv_module(cached);
            const bool backend_matches =
                compiler_backend == nullptr ||
                (info && info->backend_identity ==
                             sanitize_spirv_tag_field(compiler_backend->cache_identity()));
            std::optional<ShaderStageReflection> refl;
            bool refl_ok = (out_reflection == nullptr);
            if (!refl_ok) {
                std::vector<std::uint8_t> refl_raw;
                if (cache_vfs.read_file(refl_path, refl_raw)) {
                    refl = deserialize_shader_reflection(refl_raw, key);
                    refl_ok = refl.has_value();
                }
            }
            if (info && backend_matches && refl_ok &&
                info->generator_version == kWseSpirvGeneratorVersion &&
                info->execution_model == static_cast<std::uint32_t>(stage) &&
                info->decl_type == static_cast<std::uint32_t>(decl_type) &&
                info->source_hash == key) {
                out_words = std::move(cached);
                if (out_feature_flags != nullptr) {
                    *out_feature_flags = info->feature_flags;
                }
                if (out_reflection != nullptr) {
                    *out_reflection = std::move(*refl);
                }
                ++cache_hits;
                if (out_cache_hit != nullptr) {
                    *out_cache_hit = true;
                }
                return true;
            }
        }

        if (compiler_backend == nullptr) {
            last_error = "SpirvShaderCache has no SpirvCompilerBackend configured";
            return false;
        }

        SpirvCompileOutput compiled{};
        if (!compile_warband_glsl_to_spirv(*compiler_backend, glsl_source, stage, technique_name,
                                           decl_type, compiled, partner_glsl)) {
            last_error = std::move(compiled.info_log);
            return false;
        }
        ++compile_count;

        (void)shader_detail::ensure_vfs_dir_tree(cache_vfs, cache_dir);
        const auto write_atomic = [&cache_vfs](const std::string& dst,
                                               std::span<const std::uint8_t> bytes) {
            const std::string partial = dst + ".partial";
            if (!(cache_vfs.write_file(partial, bytes) && cache_vfs.rename_file(partial, dst))) {
                (void)cache_vfs.remove_file(partial);
                (void)cache_vfs.write_file(dst, bytes);
            }
        };
        // Sidecar first: a crash between the two writes leaves a module without a
        // matching sidecar, which is treated as a miss rather than a wrong reflection.
        write_atomic(refl_path, serialize_shader_reflection(compiled.reflection, key));
        write_atomic(path, std::span<const std::uint8_t>(
                               reinterpret_cast<const std::uint8_t*>(compiled.spirv_words.data()),
                               compiled.spirv_words.size() * sizeof(std::uint32_t)));

        if (out_feature_flags != nullptr) {
            *out_feature_flags = compiled.feature_flags;
        }
        if (out_reflection != nullptr) {
            *out_reflection = std::move(compiled.reflection);
        }
        out_words = std::move(compiled.spirv_words);
        return true;
    }
};

}  // namespace wse::asset
