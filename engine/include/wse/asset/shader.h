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
//   `varying`, loose `uniform`, `gl_FragColor`, `texture2D`) into Vulkan `#version 450`
//   GLSL using canonical vertex attribute locations, conflict-free varying locations,
//   per-stage `std140` UBOs (binding 0 VS / binding 1 FS), and sampler bindings 2..13.
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
#include <initializer_list>
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
// `OpString` tag (`wse2:...gen=3`) and baked into every cache filename so stale
// or placeholder `.spv` binaries are automatically rejected and recompiled.
inline constexpr std::uint32_t kWseSpirvGeneratorVersion = 3U;

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
};

// Graph-colored 9-location mapping covering all 39 Warband retail varying names
// with zero intra-technique conflicts even under single-stage compilation.
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

[[nodiscard]] inline std::optional<std::uint32_t> lookup_name_slot(
    std::string_view name, std::span<const NameSlotEntry> table) noexcept {
    for (const NameSlotEntry& e : table) {
        if (e.name == name) {
            return e.slot;
        }
    }
    return std::nullopt;
}

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
            if (i < src.size()) {
                out.push_back('\n');
                ++i;
            }
        } else if (c == '/' && i + 1U < src.size() && src[i + 1U] == '*') {
            i += 2U;
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

[[nodiscard]] inline std::uint32_t alloc_free_slot(std::uint64_t& used_mask,
                                                   std::uint32_t min_slot) noexcept {
    for (std::uint32_t s = min_slot; s < 64U; ++s) {
        if ((used_mask & (1ULL << s)) == 0ULL) {
            used_mask |= (1ULL << s);
            return s;
        }
    }
    return min_slot;
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
    std::uint32_t size_bytes = 0;
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

struct ShaderStageReflection {
    std::vector<ShaderUniformBlockInfo> uniform_blocks;
    std::vector<ShaderSamplerBindingInfo> samplers;
    std::vector<ShaderInputAttributeInfo> inputs;
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
};

// Translates a Warband OpenGL `#version 110/120` shader (`uniform`, `attribute`,
// `varying`, `gl_FragColor`, `texture2D`) into Vulkan `#version 450` GLSL.
[[nodiscard]] inline LoweredVulkanGlsl lower_warband_glsl_to_vulkan450(
    std::string_view glsl_source, SpirvStage stage) {
    using namespace shader_detail;
    LoweredVulkanGlsl result{};
    result.uniform_block_binding = (stage == SpirvStage::Vertex) ? 0U : 1U;

    const std::string stripped = strip_glsl_comments(glsl_source);
    if (trim_ascii(stripped).empty()) {
        result.error = "empty GLSL source";
        return result;
    }

    // Check if caller already supplied Vulkan `#version 4xx` GLSL with no legacy decls.
    {
        std::size_t pos = 0;
        while (pos < stripped.size() && (stripped[pos] == ' ' || stripped[pos] == '\t' ||
                                         stripped[pos] == '\r' || stripped[pos] == '\n')) {
            ++pos;
        }
        if (stripped.compare(pos, 8U, "#version") == 0) {
            const std::size_t eol = stripped.find('\n', pos);
            const std::string_view vline =
                (eol == std::string::npos) ? std::string_view(stripped).substr(pos)
                                           : std::string_view(stripped).substr(pos, eol - pos);
            if ((vline.find("450") != std::string::npos ||
                 vline.find("460") != std::string::npos) &&
                stripped.find("attribute ") == std::string::npos &&
                stripped.find("varying ") == std::string::npos &&
                stripped.find("gl_FragColor") == std::string::npos) {
                result.glsl = stripped;
                result.ok = true;
                return result;
            }
        }
    }

    struct DeclItem {
        std::string type;
        std::string name;
        std::string array_suffix;
        std::uint32_t slot = 0;
        bool is_bool = false;
    };

    std::vector<DeclItem> attributes;
    std::vector<DeclItem> varyings;
    std::vector<DeclItem> samplers;
    std::vector<DeclItem> ubo_members;
    std::unordered_map<std::string, bool> ubo_bool_map;
    std::string preproc_preamble;
    std::string body_raw;

    std::uint64_t used_attr_mask = 0ULL;
    for (const NameSlotEntry& e : kWarbandAttrLocations) {
        if (e.slot < 64U) used_attr_mask |= (1ULL << e.slot);
    }
    std::uint64_t used_var_mask = 0ULL;
    for (const NameSlotEntry& e : kWarbandVaryingLocations) {
        if (e.slot < 64U) used_var_mask |= (1ULL << e.slot);
    }
    std::uint64_t seen_var_mask = 0ULL;
    std::uint64_t used_sampler_mask = 3ULL;  // Reserve bindings 0 and 1 for VS/FS UBOs.
    for (const NameSlotEntry& e : kWarbandSamplerBindings) {
        if (e.slot < 64U) used_sampler_mask |= (1ULL << e.slot);
    }

    const auto try_parse_legacy_decl = [&](std::string_view stmt) -> bool {
        const std::string_view trimmed = trim_ascii(stmt);
        if (trimmed.empty() || trimmed.back() != ';') {
            return false;
        }
        std::vector<std::string_view> toks;
        std::size_t p = 0;
        while (p < trimmed.size()) {
            const char c = trimmed[p];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++p;
                continue;
            }
            if (is_glsl_ident_start(c)) {
                const std::size_t s = p++;
                while (p < trimmed.size() && is_glsl_ident_char(trimmed[p])) ++p;
                toks.push_back(trimmed.substr(s, p - s));
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                const std::size_t s = p++;
                while (p < trimmed.size() && is_glsl_ident_char(trimmed[p])) ++p;
                toks.push_back(trimmed.substr(s, p - s));
                continue;
            }
            toks.push_back(trimmed.substr(p, 1U));
            ++p;
        }
        if (toks.size() < 4U) {
            return false;
        }
        const std::string_view qual = toks[0];
        if (qual != "uniform" && qual != "attribute" && qual != "varying") {
            return false;
        }
        std::size_t idx = 1U;
        if (toks[idx] == "highp" || toks[idx] == "mediump" || toks[idx] == "lowp") {
            ++idx;
        }
        if (idx + 2U >= toks.size()) {
            return false;
        }
        const std::string_view type = toks[idx++];
        while (idx < toks.size()) {
            if (toks[idx] == ";") {
                break;
            }
            const std::string_view name = toks[idx++];
            std::string arr;
            if (idx < toks.size() && toks[idx] == "[") {
                ++idx;
                arr.push_back('[');
                while (idx < toks.size() && toks[idx] != "]") {
                    arr.append(toks[idx++]);
                }
                if (idx < toks.size() && toks[idx] == "]") {
                    arr.push_back(']');
                    ++idx;
                }
            }
            if (qual == "attribute") {
                if (stage != SpirvStage::Vertex) {
                    result.error = "attribute declaration not valid in fragment shader";
                    return true;
                }
                const auto fixed = warband_vertex_attribute_location(name);
                const std::uint32_t loc = fixed ? *fixed : alloc_free_slot(used_attr_mask, 9U);
                attributes.push_back({std::string(type), std::string(name), arr, loc, false});
            } else if (qual == "varying") {
                const auto fixed = warband_varying_location(name);
                const std::uint32_t loc = fixed ? *fixed : alloc_free_slot(used_var_mask, 9U);
                if (loc < 64U) {
                    if ((seen_var_mask & (1ULL << loc)) != 0ULL) {
                        result.error = "varying location conflict for " + std::string(name);
                        return true;
                    }
                    seen_var_mask |= (1ULL << loc);
                }
                varyings.push_back({std::string(type), std::string(name), arr, loc, false});
            } else if (type.starts_with("sampler")) {
                const auto fixed = warband_sampler_binding(name);
                const std::uint32_t bind =
                    fixed ? *fixed : alloc_free_slot(used_sampler_mask, 14U);
                samplers.push_back({std::string(type), std::string(name), arr, bind, false});
            } else {
                const bool is_bool = (type == "bool");
                if (ubo_bool_map.emplace(std::string(name), is_bool).second) {
                    ubo_members.push_back(
                        {std::string(type), std::string(name), arr, 0U, is_bool});
                }
            }
            if (idx < toks.size() && toks[idx] == ",") {
                ++idx;
            }
        }
        return true;
    };

    // Pass 1: separate top-level preprocessor lines, legacy decls, and executable body.
    {
        std::size_t i = 0;
        int brace_depth = 0;
        std::size_t stmt_start = 0;
        bool at_line_start = true;
        while (i < stripped.size()) {
            const char c = stripped[i];
            if (brace_depth == 0 && at_line_start && (c == ' ' || c == '\t')) {
                ++i;
                continue;
            }
            if (brace_depth == 0 && at_line_start && c == '#') {
                if (i > stmt_start) {
                    body_raw.append(stripped.substr(stmt_start, i - stmt_start));
                }
                const std::size_t eol = stripped.find('\n', i);
                const std::size_t next_i = (eol == std::string::npos) ? stripped.size() : (eol + 1U);
                const std::string_view line = std::string_view(stripped).substr(i, next_i - i);
                if (!line.starts_with("#version")) {
                    preproc_preamble.append(line);
                    if (line.back() != '\n') preproc_preamble.push_back('\n');
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
                    result.error = "unmatched '}' in GLSL source";
                    return result;
                }
                --brace_depth;
                if (brace_depth == 0) {
                    body_raw.append(stripped.substr(stmt_start, i + 1U - stmt_start));
                    stmt_start = i + 1U;
                }
            } else if (c == ';' && brace_depth == 0) {
                const std::string_view stmt =
                    std::string_view(stripped).substr(stmt_start, i + 1U - stmt_start);
                if (!try_parse_legacy_decl(stmt)) {
                    body_raw.append(stmt);
                }
                if (!result.error.empty()) {
                    return result;
                }
                stmt_start = i + 1U;
            }
            ++i;
        }
        if (brace_depth != 0) {
            result.error = "unclosed '{' in GLSL source";
            return result;
        }
        if (stmt_start < stripped.size()) {
            body_raw.append(stripped.substr(stmt_start));
        }
    }

    // Pass 2: rewrite identifier references in `body_raw`.
    const std::string_view ubo_inst =
        (stage == SpirvStage::Vertex) ? "_wse_vs_u" : "_wse_fs_u";
    bool uses_frag_color = false;
    bool saw_main = false;
    std::string rewritten_body;
    rewritten_body.reserve(body_raw.size() + 256U);
    {
        std::size_t i = 0;
        char prev_non_ws = '\0';
        bool at_line_start = true;
        while (i < body_raw.size()) {
            const char c = body_raw[i];
            if (at_line_start && (c == ' ' || c == '\t')) {
                rewritten_body.push_back(c);
                ++i;
                continue;
            }
            if (at_line_start && c == '#') {
                const std::size_t eol = body_raw.find('\n', i);
                const std::size_t next_i = (eol == std::string::npos) ? body_raw.size() : (eol + 1U);
                rewritten_body.append(body_raw.substr(i, next_i - i));
                i = next_i;
                at_line_start = true;
                prev_non_ws = '\n';
                continue;
            }
            at_line_start = (c == '\n');
            if (is_glsl_ident_start(c)) {
                const std::size_t s = i++;
                while (i < body_raw.size() && is_glsl_ident_char(body_raw[i])) ++i;
                const std::string_view tok(body_raw.data() + s, i - s);
                if (prev_non_ws == '.') {
                    rewritten_body.append(tok);
                } else {
                    if (tok == "main") saw_main = true;
                    if (tok == "texture2D") {
                        rewritten_body.append("texture");
                    } else if (tok == "texture2DProj") {
                        rewritten_body.append("textureProj");
                    } else if (tok == "texture2DLod") {
                        rewritten_body.append("textureLod");
                    } else if (tok == "shadow2D") {
                        rewritten_body.append("texture");
                    } else if (stage == SpirvStage::Fragment && tok == "gl_FragColor") {
                        uses_frag_color = true;
                        rewritten_body.append("_wse_frag_color");
                    } else {
                        const auto it = ubo_bool_map.find(std::string(tok));
                        if (it != ubo_bool_map.end()) {
                            if (it->second) {
                                rewritten_body.push_back('(');
                                rewritten_body.append(ubo_inst);
                                rewritten_body.append("._wse_b_");
                                rewritten_body.append(tok);
                                rewritten_body.append(" != 0u)");
                            } else {
                                rewritten_body.append(ubo_inst);
                                rewritten_body.append("._wse_u_");
                                rewritten_body.append(tok);
                            }
                        } else {
                            rewritten_body.append(tok);
                        }
                    }
                }
                prev_non_ws = 'A';
            } else {
                rewritten_body.push_back(c);
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                    prev_non_ws = c;
                }
                ++i;
            }
        }
    }

    if (!saw_main) {
        result.error = "GLSL source missing main() entry point";
        return result;
    }

    std::string out;
    out.reserve(rewritten_body.size() + 1024U);
    out.append("#version 450\n");
    if (!preproc_preamble.empty()) {
        out.append(preproc_preamble);
    }
    for (const DeclItem& d : attributes) {
        out.append("layout(location = ");
        out.append(std::to_string(d.slot));
        out.append(") in ");
        out.append(d.type);
        out.push_back(' ');
        out.append(d.name);
        out.append(d.array_suffix);
        out.append(";\n");
    }
    const char* var_qual = (stage == SpirvStage::Vertex) ? "out " : "in ";
    for (const DeclItem& d : varyings) {
        out.append("layout(location = ");
        out.append(std::to_string(d.slot));
        out.append(") ");
        out.append(var_qual);
        out.append(d.type);
        out.push_back(' ');
        out.append(d.name);
        out.append(d.array_suffix);
        out.append(";\n");
    }
    if (stage == SpirvStage::Fragment && uses_frag_color) {
        out.append("layout(location = 0) out vec4 _wse_frag_color;\n");
    }
    for (const DeclItem& d : samplers) {
        out.append("layout(set = 0, binding = ");
        out.append(std::to_string(d.slot));
        out.append(") uniform ");
        out.append(d.type);
        out.push_back(' ');
        out.append(d.name);
        out.append(d.array_suffix);
        out.append(";\n");
    }
    if (!ubo_members.empty()) {
        out.append("layout(set = 0, binding = ");
        out.append(std::to_string(result.uniform_block_binding));
        out.append(", std140) uniform ");
        out.append(stage == SpirvStage::Vertex ? "WseVertexUniforms {\n"
                                               : "WseFragmentUniforms {\n");
        for (const DeclItem& d : ubo_members) {
            out.append("    ");
            if (d.is_bool) {
                out.append("uint _wse_b_");
            } else {
                out.append(d.type);
                out.append(" _wse_u_");
            }
            out.append(d.name);
            out.append(d.array_suffix);
            out.append(";\n");
        }
        out.append("} ");
        out.append(ubo_inst);
        out.append(";\n");
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
    info.decl_type = static_cast<std::uint32_t>(std::stoul(std::string(decl_text)));
    info.feature_flags = static_cast<std::uint32_t>(*feat);
    info.source_hash = *src;
    info.generator_version = static_cast<std::uint32_t>(std::stoul(std::string(gen_text)));
    return info;
}

[[nodiscard]] inline bool compile_warband_glsl_to_spirv(
    SpirvCompilerBackend& backend,
    std::string_view glsl_source,
    SpirvStage stage,
    std::string_view technique_name,
    VertexDeclType decl_type,
    SpirvCompileOutput& out) {
    using namespace shader_detail;
    out = {};
    out.feature_flags = detect_glsl_feature_flags(glsl_source, decl_type);

    LoweredVulkanGlsl lowered = lower_warband_glsl_to_vulkan450(glsl_source, stage);
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

    std::string clean_tech;
    clean_tech.reserve(technique_name.size());
    for (char c : technique_name) {
        clean_tech.push_back((c == ':' || c == '\0') ? '_' : c);
    }
    const std::string tag = "wse2:tech=" + clean_tech +
                            ":decl=" + std::to_string(static_cast<int>(decl_type)) +
                            ":feat=" + hex_u64(out.feature_flags) +
                            ":src=" + hex_u64(fnv1a64(glsl_source)) +
                            ":gen=" + std::to_string(kWseSpirvGeneratorVersion);
    if (!inject_spirv_tag_opstring(spv, tag) || !inspect_spirv_module(spv)) {
        out.info_log = "emitted SPIR-V failed post-tag validation";
        return false;
    }
    out.spirv_words = std::move(spv);
    return true;
}

class SpirvShaderCache {
public:
    std::shared_ptr<SpirvCompilerBackend> compiler_backend;
    std::size_t cache_hits = 0;
    std::size_t compile_count = 0;
    std::string last_error;

    explicit SpirvShaderCache(std::shared_ptr<SpirvCompilerBackend> backend = nullptr)
        : compiler_backend(std::move(backend)) {}

    [[nodiscard]] static std::string compute_cache_filename(std::string_view technique_name,
                                                            SpirvStage stage,
                                                            VertexDeclType decl_type,
                                                            std::string_view glsl_source) {
        std::uint64_t h = shader_detail::fnv1a64(
            "wse2_spv_gen" + std::to_string(kWseSpirvGeneratorVersion) + ":");
        h = shader_detail::fnv1a64(wse::platform::ascii_lower(technique_name), h);
        h = shader_detail::fnv1a64(stage == SpirvStage::Vertex ? ":vs:" : ":fs:", h);
        h = shader_detail::fnv1a64(std::to_string(static_cast<int>(decl_type)), h);
        h = shader_detail::fnv1a64(":", h);
        h = shader_detail::fnv1a64(glsl_source, h);
        return shader_detail::hex_u64(h) + ".spv";
    }

    // Returns the cached SPIR-V 1.0 module when it validates completely and its `OpString`
    // tag matches this source, stage, vertex declaration, and generator version; otherwise
    // lowers & compiles through `compiler_backend` and persists via `<file>.partial` + `rename_file`.
    [[nodiscard]] bool get_or_compile(wse::platform::Vfs& cache_vfs,
                                      std::string_view cache_dir,
                                      std::string_view technique_name,
                                      SpirvStage stage,
                                      VertexDeclType decl_type,
                                      std::string_view glsl_source,
                                      std::vector<std::uint32_t>& out_words,
                                      bool* out_cache_hit = nullptr,
                                      std::uint32_t* out_feature_flags = nullptr,
                                      ShaderStageReflection* out_reflection = nullptr) {
        out_words.clear();
        last_error.clear();
        if (out_cache_hit != nullptr) {
            *out_cache_hit = false;
        }
        const std::string fname = compute_cache_filename(technique_name, stage, decl_type, glsl_source);
        const std::string path = wse::game::vfs_join_path(cache_dir, fname);

        std::vector<std::uint8_t> raw;
        if (cache_vfs.read_file(path, raw) && raw.size() >= 24U && (raw.size() % 4U) == 0U) {
            std::vector<std::uint32_t> cached(raw.size() / 4U);
            std::memcpy(cached.data(), raw.data(), raw.size());
            const std::optional<SpirvModuleInfo> info = inspect_spirv_module(cached);
            if (info && info->generator_version == kWseSpirvGeneratorVersion &&
                info->execution_model == static_cast<std::uint32_t>(stage) &&
                info->decl_type == static_cast<std::uint32_t>(decl_type) &&
                info->source_hash == shader_detail::fnv1a64(glsl_source)) {
                out_words = std::move(cached);
                if (out_feature_flags != nullptr) {
                    *out_feature_flags = info->feature_flags;
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
                                           decl_type, compiled)) {
            last_error = std::move(compiled.info_log);
            return false;
        }
        if (out_feature_flags != nullptr) {
            *out_feature_flags = compiled.feature_flags;
        }
        if (out_reflection != nullptr) {
            *out_reflection = std::move(compiled.reflection);
        }
        out_words = std::move(compiled.spirv_words);
        ++compile_count;

        (void)shader_detail::ensure_vfs_dir_tree(cache_vfs, cache_dir);
        const std::span<const std::uint8_t> spv_bytes(
            reinterpret_cast<const std::uint8_t*>(out_words.data()),
            out_words.size() * sizeof(std::uint32_t));
        const std::string partial = path + ".partial";
        if (!(cache_vfs.write_file(partial, spv_bytes) && cache_vfs.rename_file(partial, path))) {
            (void)cache_vfs.remove_file(partial);
            (void)cache_vfs.write_file(path, spv_bytes);
        }
        return true;
    }
};

}  // namespace wse::asset
