// Clean-room Phase 2G Warband/WSE2 .brf / .dds asset inspection CLI.
//
// Modes:
//   wse_brf --brf <file.brf> [--json]
//   wse_brf --dds <file.dds> [--json]
//   wse_brf --dir <dir> [--json]
//   wse_brf --root <warband_root> [--module <name>] [--compile-shaders [--jobs N]] [--json]

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/asset/shader.h"
#include "wse/asset/shader_compiler.h"
#include "wse/game/module_loader_common.h"
#include "wse/platform/vfs.h"

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s --brf <file.brf> [--json]\n"
                 "  %s --dds <file.dds> [--json]\n"
                 "  %s --dir <dir> [--json]\n"
                 "  %s --root <warband_root> [--module <name>] [--compile-shaders [--jobs N]] [--json]\n",
                 argv0 != nullptr ? argv0 : "wse_brf",
                 argv0 != nullptr ? argv0 : "wse_brf",
                 argv0 != nullptr ? argv0 : "wse_brf",
                 argv0 != nullptr ? argv0 : "wse_brf");
}

const char* dds_format_name(wse::asset::DdsFormat fmt) noexcept {
    switch (fmt) {
        case wse::asset::DdsFormat::BC1_UNORM: return "BC1_UNORM";
        case wse::asset::DdsFormat::BC2_UNORM: return "BC2_UNORM";
        case wse::asset::DdsFormat::BC3_UNORM: return "BC3_UNORM";
        case wse::asset::DdsFormat::BC4_UNORM: return "BC4_UNORM";
        case wse::asset::DdsFormat::BC5_UNORM: return "BC5_UNORM";
        case wse::asset::DdsFormat::B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case wse::asset::DdsFormat::R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case wse::asset::DdsFormat::B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
        case wse::asset::DdsFormat::R8G8B8X8_UNORM: return "R8G8B8X8_UNORM";
        case wse::asset::DdsFormat::B8G8R8_UNORM: return "B8G8R8_UNORM";
        case wse::asset::DdsFormat::B5G6R5_UNORM: return "B5G6R5_UNORM";
        case wse::asset::DdsFormat::B5G5R5A1_UNORM: return "B5G5R5A1_UNORM";
        case wse::asset::DdsFormat::B4G4R4A4_UNORM: return "B4G4R4A4_UNORM";
        case wse::asset::DdsFormat::A8_UNORM: return "A8_UNORM";
        case wse::asset::DdsFormat::L8_UNORM: return "L8_UNORM";
        case wse::asset::DdsFormat::L16_UNORM: return "L16_UNORM";
        case wse::asset::DdsFormat::A8L8_UNORM: return "A8L8_UNORM";
        default: return "Unknown";
    }
}

void print_json_escaped(std::string_view s) {
    std::putchar('"');
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            std::printf("\\%c", static_cast<char>(c));
        } else if (c < 0x20U) {
            std::printf("\\u%04x", static_cast<unsigned>(c));
        } else {
            std::putchar(static_cast<char>(c));
        }
    }
    std::putchar('"');
}

struct BrfTotals {
    std::size_t files = 0;
    std::size_t meshes = 0;
    std::size_t textures = 0;
    std::size_t shaders = 0;
    std::size_t materials = 0;
    std::size_t bodies = 0;
    std::size_t skeletons = 0;
    std::size_t animations = 0;
    std::size_t physics_materials = 0;
    std::size_t meta_meshes = 0;

    void add(const wse::asset::BrfFile& f) {
        ++files;
        meshes += f.meshes.size();
        textures += f.textures.size();
        shaders += f.shaders.size();
        materials += f.materials.size();
        bodies += f.bodies.size();
        skeletons += f.skeletons.size();
        animations += f.animations.size();
        physics_materials += f.physics_materials.size();
        meta_meshes += wse::asset::group_meta_meshes(f.meshes).size();
    }
};

bool load_host_file(const std::filesystem::path& file_path,
                    std::vector<std::uint8_t>& out,
                    std::string& err) {
    std::filesystem::path dir = file_path.parent_path();
    if (dir.empty()) dir = ".";
    wse::platform::HostVfs vfs(dir);
    const std::string name = file_path.filename().string();
    if (!vfs.read_file(name, out)) {
        err = "unable to open " + file_path.string();
        return false;
    }
    return true;
}

int run_brf_file(const std::filesystem::path& p, bool json) {
    std::vector<std::uint8_t> raw;
    std::string err;
    if (!load_host_file(p, raw, err)) {
        std::fprintf(stderr, "ERROR %s\n", err.c_str());
        return 1;
    }
    wse::asset::BrfFile brf;
    if (!wse::asset::parse_brf(std::as_bytes(std::span<const std::uint8_t>(raw)), brf, &err)) {
        std::fprintf(stderr, "ERROR %s: %s\n", p.string().c_str(), err.c_str());
        return 1;
    }
    const auto groups = wse::asset::group_meta_meshes(brf.meshes);
    if (json) {
        std::printf("{\"path\":");
        print_json_escaped(p.string());
        std::printf(",\"rfver\":%d,\"meshes\":%zu,\"meta_meshes\":%zu,\"textures\":%zu,"
                    "\"shaders\":%zu,\"materials\":%zu,\"bodies\":%zu,\"skeletons\":%zu,"
                    "\"animations\":%zu,\"physics_materials\":%zu}\n",
                    brf.rfver, brf.meshes.size(), groups.size(), brf.textures.size(),
                    brf.shaders.size(), brf.materials.size(), brf.bodies.size(),
                    brf.skeletons.size(), brf.animations.size(), brf.physics_materials.size());
    } else {
        std::printf("BRF %s: rfver=%d meshes=%zu meta=%zu tex=%zu shader=%zu mat=%zu body=%zu skel=%zu anim=%zu phys=%zu\n",
                    p.string().c_str(), brf.rfver, brf.meshes.size(), groups.size(),
                    brf.textures.size(), brf.shaders.size(), brf.materials.size(),
                    brf.bodies.size(), brf.skeletons.size(), brf.animations.size(),
                    brf.physics_materials.size());
    }
    return 0;
}

int run_dds_file(const std::filesystem::path& p, bool json) {
    std::vector<std::uint8_t> raw;
    std::string err;
    if (!load_host_file(p, raw, err)) {
        std::fprintf(stderr, "ERROR %s\n", err.c_str());
        return 1;
    }
    wse::asset::DdsImageView img;
    if (!wse::asset::parse_dds(std::as_bytes(std::span<const std::uint8_t>(raw)), img, &err)) {
        std::fprintf(stderr, "ERROR %s: %s\n", p.string().c_str(), err.c_str());
        return 1;
    }
    std::vector<std::uint8_t> rgba;
    if (!wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba, &err)) {
        std::fprintf(stderr, "ERROR %s: %s\n", p.string().c_str(), err.c_str());
        return 1;
    }
    if (json) {
        std::printf("{\"path\":");
        print_json_escaped(p.string());
        std::printf(",\"width\":%u,\"height\":%u,\"mips\":%u,\"faces\":%u,"
                    "\"cubemap\":%s,\"dx10\":%s,\"format\":\"%s\",\"srgb\":%s,\"mip0_rgba_bytes\":%zu}\n",
                    img.width, img.height, img.mip_count, img.face_count,
                    img.is_cubemap ? "true" : "false", img.has_dx10_header ? "true" : "false",
                    dds_format_name(img.format), img.is_srgb ? "true" : "false", rgba.size());
    } else {
        std::printf("DDS %s: %ux%u format=%s%s mips=%u faces=%u cubemap=%s dx10=%s decoded=%zu B\n",
                    p.string().c_str(), img.width, img.height, dds_format_name(img.format),
                    img.is_srgb ? " (sRGB)" : "", img.mip_count, img.face_count,
                    img.is_cubemap ? "yes" : "no", img.has_dx10_header ? "yes" : "no",
                    rgba.size());
    }
    return 0;
}

int run_dir(const std::filesystem::path& dir, bool json) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        std::fprintf(stderr, "ERROR not a directory: %s\n", dir.string().c_str());
        return 1;
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = wse::platform::ascii_lower(it->path().extension().string());
        if (ext == ".brf" || ext == ".dds") {
            files.push_back(it->path());
        }
    }
    if (ec) {
        std::fprintf(stderr, "ERROR scanning directory: %s\n", ec.message().c_str());
        return 1;
    }
    std::sort(files.begin(), files.end());

    BrfTotals totals{};
    std::size_t brf_fail = 0;
    std::size_t dds_ok = 0;
    std::size_t dds_fail = 0;
    std::vector<std::uint8_t> raw;
    std::vector<std::uint8_t> rgba;

    for (const auto& p : files) {
        const std::string ext = wse::platform::ascii_lower(p.extension().string());
        std::string err;
        if (!load_host_file(p, raw, err)) {
            if (ext == ".brf") ++brf_fail; else ++dds_fail;
            std::fprintf(stderr, "FAIL  %s: %s\n", p.string().c_str(), err.c_str());
            continue;
        }
        const auto bytes = std::as_bytes(std::span<const std::uint8_t>(raw));
        if (ext == ".brf") {
            wse::asset::BrfFile brf;
            if (!wse::asset::parse_brf(bytes, brf, &err)) {
                ++brf_fail;
                std::fprintf(stderr, "FAIL  %s: %s\n", p.string().c_str(), err.c_str());
                continue;
            }
            totals.add(brf);
        } else {
            wse::asset::DdsImageView img;
            if (!wse::asset::parse_dds(bytes, img, &err) ||
                !wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba, &err)) {
                ++dds_fail;
                std::fprintf(stderr, "FAIL  %s: %s\n", p.string().c_str(), err.c_str());
                continue;
            }
            ++dds_ok;
        }
    }

    if (json) {
        std::printf("{\"dir\":");
        print_json_escaped(dir.string());
        std::printf(",\"brf_ok\":%zu,\"brf_fail\":%zu,\"dds_ok\":%zu,\"dds_fail\":%zu,"
                    "\"meshes\":%zu,\"meta_meshes\":%zu,\"textures\":%zu,\"shaders\":%zu,"
                    "\"materials\":%zu,\"bodies\":%zu,\"skeletons\":%zu,\"animations\":%zu,"
                    "\"physics_materials\":%zu}\n",
                    totals.files, brf_fail, dds_ok, dds_fail,
                    totals.meshes, totals.meta_meshes, totals.textures, totals.shaders,
                    totals.materials, totals.bodies, totals.skeletons, totals.animations,
                    totals.physics_materials);
    } else {
        std::printf("DIR %s: brf_ok=%zu brf_fail=%zu dds_ok=%zu dds_fail=%zu "
                    "(meshes=%zu meta=%zu tex=%zu shader=%zu mat=%zu body=%zu skel=%zu anim=%zu phys=%zu)\n",
                    dir.string().c_str(), totals.files, brf_fail, dds_ok, dds_fail,
                    totals.meshes, totals.meta_meshes, totals.textures, totals.shaders,
                    totals.materials, totals.bodies, totals.skeletons, totals.animations,
                    totals.physics_materials);
    }
    return (brf_fail == 0 && dds_fail == 0) ? 0 : 1;
}

int run_module_catalog(const std::filesystem::path& root,
                       std::string_view module_name,
                       bool json,
                       bool compile_shaders,
                       unsigned jobs) {
    wse::platform::HostVfs vfs(root);
    std::string mod_rel(module_name);
    if (mod_rel.find('/') == std::string::npos && mod_rel.find('\\') == std::string::npos) {
        mod_rel = "Modules/" + mod_rel;
    }
    const std::string ini_path = wse::game::vfs_join_path(mod_rel, "module.ini");
    std::string ini_text;
    if (!vfs.read_text(ini_path, ini_text)) {
        std::fprintf(stderr, "ERROR unable to read %s under %s\n",
                     ini_path.c_str(), root.string().c_str());
        return 1;
    }
    wse::game::ModuleIniData ini{};
    wse::game::parse_module_ini(ini_text, ini);
    wse::asset::BrfCatalog cat;
    std::string err;
    if (!wse::asset::load_module_brf_catalog(vfs, "", mod_rel, ini, cat, &err)) {
        std::fprintf(stderr, "ERROR %s\n", err.c_str());
        return 1;
    }
    BrfTotals totals{};
    std::size_t loaded_entries = 0;
    for (const auto& entry : cat.entries) {
        if (entry.loaded) {
            ++loaded_entries;
            totals.add(entry.file);
        }
    }
    for (const std::string& warning : cat.warnings) {
        std::fprintf(stderr, "WARN  %s\n", warning.c_str());
    }

    // Totals are already counted, so the shaders are moved (not copied) out of the
    // catalog; the first definition of a name wins.
    std::unordered_map<std::string, wse::asset::BrfShader> shaders_by_lower;
    for (auto& entry : cat.entries) {
        if (!entry.loaded) {
            continue;
        }
        for (wse::asset::BrfShader& sh : entry.file.shaders) {
            shaders_by_lower.try_emplace(wse::platform::ascii_lower(sh.name), std::move(sh));
        }
    }
    wse::asset::BrfFile opt_wse2_shaders{};
    if (wse::asset::load_optional_wse2_shaders_brf(vfs, "", mod_rel, opt_wse2_shaders)) {
        for (wse::asset::BrfShader& sh : opt_wse2_shaders.shaders) {
            shaders_by_lower.try_emplace(wse::platform::ascii_lower(sh.name), std::move(sh));
        }
    }

    // Only technique names are needed unless `--compile-shaders` is passed.
    wse::asset::ShaderTechniqueCatalog tech_cat{};
    std::unordered_set<std::string> tech_names_lower;
    wse::asset::TechniqueCatalogOptions tech_opts{};
    tech_opts.load_glsl_sources = compile_shaders;
    if (wse::asset::load_warband_shader_technique_catalog(vfs, "", tech_cat, nullptr, tech_opts)) {
        tech_names_lower.reserve(tech_cat.techniques_by_lower.size());
        for (const auto& kv : tech_cat.techniques_by_lower) {
            tech_names_lower.insert(kv.first);
        }
    }
    const std::unordered_set<std::string>* tech_ptr =
        tech_names_lower.empty() ? nullptr : &tech_names_lower;

    wse::asset::ShaderQualityConfig quality_cfg{};
    quality_cfg.require_hlsl_technique = !tech_names_lower.empty();

    wse::asset::ShaderPairTable raw_pairs{};
    const std::string shader_ini_path =
        wse::asset::resolve_wse2_shaders_ini_path(vfs, "", mod_rel);
    std::string shader_ini_text;
    if (!shader_ini_path.empty() && vfs.read_text(shader_ini_path, shader_ini_text)) {
        wse::asset::parse_wse2_shaders_ini(shader_ini_text, raw_pairs);
    }
    wse::asset::ShaderPairTable valid_pairs{};
    wse::asset::validate_shader_pairs(raw_pairs, shaders_by_lower, quality_cfg,
                                      valid_pairs, tech_ptr);

    wse::asset::SuitableShaderResolver resolver(shaders_by_lower, quality_cfg, tech_ptr);
    std::size_t shaders_suitable = 0;
    std::size_t shaders_fallback = 0;
    for (const auto& kv : shaders_by_lower) {
        const wse::asset::ShaderResolutionResult& res = resolver.resolve(kv.first);
        if (res.shader != nullptr) {
            if (res.hop_depth == 0 && !res.used_default_fallback) {
                ++shaders_suitable;
            } else {
                ++shaders_fallback;
            }
        }
    }

    std::size_t materials_resolved = 0;
    std::size_t materials_alpha_test = 0;
    std::size_t materials_blended = 0;
    for (const auto& entry : cat.entries) {
        if (!entry.loaded) {
            continue;
        }
        for (const wse::asset::BrfMaterial& mat : entry.file.materials) {
            const wse::asset::ShaderResolutionResult& res = resolver.resolve(mat.shader_name);
            const wse::asset::MaterialRenderState state =
                wse::asset::decode_material_render_state(mat, res.shader);
            if (res.shader != nullptr) {
                ++materials_resolved;
            }
            if (state.alpha_ref_u8 != 0U) {
                ++materials_alpha_test;
            }
            if (state.blend_mode != wse::asset::MaterialBlendMode::Opaque) {
                ++materials_blended;
            }
        }
    }

    std::size_t spirv_ok = 0;
    std::size_t spirv_fail = 0;
    if (compile_shaders) {
        // Sorted, so reports and diffs between runs are stable; compiled on `jobs`
        // threads sharing one backend (glslang compiles are thread-safe).
        std::vector<const wse::asset::GlTechniqueEntry*> techs;
        techs.reserve(tech_cat.techniques_by_lower.size());
        for (const auto& kv : tech_cat.techniques_by_lower) {
            techs.push_back(&kv.second);
        }
        std::sort(techs.begin(), techs.end(),
                  [](const auto* a, const auto* b) { return a->name < b->name; });
        struct TechResult {
            bool ok = false;
            std::string vs_error;
            std::string fs_error;
        };
        std::vector<TechResult> results(techs.size());
        const auto compiler = wse::asset::create_glslang_shader_compiler();
        std::atomic<std::size_t> next{0};
        const auto worker = [&]() {
            for (std::size_t i = next.fetch_add(1); i < techs.size(); i = next.fetch_add(1)) {
                const wse::asset::GlTechniqueEntry& tech = *techs[i];
                TechResult& r = results[i];
                if (tech.vs_glsl.empty() || tech.fs_glsl.empty()) {
                    (tech.vs_glsl.empty() ? r.vs_error : r.fs_error) = "missing GLSL source";
                    continue;
                }
                // No mesh here, so take the declaration the vertex shader's attributes ask for.
                const wse::asset::VertexDeclType decl =
                    wse::asset::infer_vertex_decl_from_glsl(tech.vs_glsl);
                wse::asset::SpirvCompileOutput vs_out{};
                wse::asset::SpirvCompileOutput fs_out{};
                const bool vs_ok = wse::asset::compile_warband_glsl_to_spirv(
                    *compiler, tech.vs_glsl, wse::asset::SpirvStage::Vertex, tech.name, decl,
                    vs_out, tech.fs_glsl);
                const bool fs_ok = wse::asset::compile_warband_glsl_to_spirv(
                    *compiler, tech.fs_glsl, wse::asset::SpirvStage::Fragment, tech.name, decl,
                    fs_out, tech.vs_glsl);
                r.ok = vs_ok && fs_ok;
                if (!vs_ok) {
                    r.vs_error = std::move(vs_out.info_log);
                }
                if (!fs_ok) {
                    r.fs_error = std::move(fs_out.info_log);
                }
            }
        };
        const unsigned threads = std::max(1U, std::min<unsigned>(
                                                  jobs, static_cast<unsigned>(techs.size())));
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < threads; ++t) {
            pool.emplace_back(worker);
        }
        worker();
        for (std::thread& th : pool) {
            th.join();
        }
        for (std::size_t i = 0; i < techs.size(); ++i) {
            const TechResult& r = results[i];
            if (r.ok) {
                ++spirv_ok;
                continue;
            }
            ++spirv_fail;
            if (!r.vs_error.empty()) {
                std::fprintf(stderr, "SHADER_FAIL %s (vertex): %s\n", techs[i]->name.c_str(),
                             r.vs_error.c_str());
            }
            if (!r.fs_error.empty()) {
                std::fprintf(stderr, "SHADER_FAIL %s (fragment): %s\n", techs[i]->name.c_str(),
                             r.fs_error.c_str());
            }
        }
    }

    if (json) {
        std::printf("{\"root\":");
        print_json_escaped(root.string());
        std::printf(",\"module\":");
        print_json_escaped(mod_rel);
        std::printf(",\"warning_list\":[");
        for (std::size_t i = 0; i < cat.warnings.size(); ++i) {
            if (i != 0U) std::putchar(',');
            print_json_escaped(cat.warnings[i]);
        }
        std::printf("]");
        std::printf(",\"entries\":%zu,\"loaded\":%zu,\"warnings\":%zu,"
                    "\"meshes\":%zu,\"meta_meshes\":%zu,\"textures\":%zu,\"shaders\":%zu,"
                    "\"materials\":%zu,\"bodies\":%zu,\"skeletons\":%zu,\"animations\":%zu,"
                    "\"physics_materials\":%zu,"
                    "\"gl_techniques\":%zu,\"spirv_ok\":%zu,\"spirv_fail\":%zu,"
                    "\"shaders_suitable\":%zu,\"shaders_fallback\":%zu,"
                    "\"materials_resolved\":%zu,\"materials_alpha_test\":%zu,"
                    "\"materials_blended\":%zu,\"wse2_ini_fallbacks\":%zu}\n",
                    cat.entries.size(), loaded_entries, cat.warnings.size(),
                    totals.meshes, totals.meta_meshes, totals.textures, totals.shaders,
                    totals.materials, totals.bodies, totals.skeletons, totals.animations,
                    totals.physics_materials,
                    tech_cat.techniques_by_lower.size(), spirv_ok, spirv_fail,
                    shaders_suitable, shaders_fallback,
                    materials_resolved, materials_alpha_test, materials_blended,
                    valid_pairs.entries.size());
    } else {
        std::printf("CATALOG %s (%s): entries=%zu loaded=%zu warnings=%zu "
                    "(meshes=%zu meta=%zu tex=%zu shader=%zu mat=%zu body=%zu skel=%zu anim=%zu phys=%zu) "
                    "[gl_tech=%zu spv_ok=%zu spv_fail=%zu shaders_ok=%zu fallback=%zu "
                    "mat_resolved=%zu mat_alpha_test=%zu mat_blended=%zu ini_pairs=%zu]\n",
                    root.string().c_str(), mod_rel.c_str(), cat.entries.size(), loaded_entries,
                    cat.warnings.size(), totals.meshes, totals.meta_meshes, totals.textures,
                    totals.shaders, totals.materials, totals.bodies, totals.skeletons,
                    totals.animations, totals.physics_materials,
                    tech_cat.techniques_by_lower.size(), spirv_ok, spirv_fail,
                    shaders_suitable, shaders_fallback,
                    materials_resolved, materials_alpha_test, materials_blended,
                    valid_pairs.entries.size());
    }
    return spirv_fail == 0U ? 0 : 1;
}

int run_cli(int argc, char** argv) {
    std::string brf_path;
    std::string dds_path;
    std::string dir_path;
    std::string root_path;
    std::string module_name = "Native";
    bool json = false;
    bool compile_shaders = false;
    unsigned jobs = std::max(1U, std::thread::hardware_concurrency());

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--json") {
            json = true;
            continue;
        }
        if (arg == "--compile-shaders") {
            compile_shaders = true;
            continue;
        }
        const auto need_val = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value after %.*s\n",
                             static_cast<int>(arg.size()), arg.data());
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--brf") {
            if (!need_val(brf_path)) return 2;
        } else if (arg == "--dds") {
            if (!need_val(dds_path)) return 2;
        } else if (arg == "--dir") {
            if (!need_val(dir_path)) return 2;
        } else if (arg == "--root") {
            if (!need_val(root_path)) return 2;
        } else if (arg == "--module") {
            if (!need_val(module_name)) return 2;
        } else if (arg == "--jobs") {
            std::string v;
            if (!need_val(v)) return 2;
            char* end = nullptr;
            const unsigned long n = std::strtoul(v.c_str(), &end, 10);
            if (v.empty() || end == nullptr || *end != '\0' || n == 0UL || n > 256UL) {
                std::fprintf(stderr, "--jobs expects an integer in 1..256\n");
                return 2;
            }
            jobs = static_cast<unsigned>(n);
        } else {
            std::fprintf(stderr, "unknown option: %.*s\n",
                         static_cast<int>(arg.size()), arg.data());
            print_usage(argv[0]);
            return 2;
        }
    }

    const int modes = (!brf_path.empty() ? 1 : 0) + (!dds_path.empty() ? 1 : 0) +
                      (!dir_path.empty() ? 1 : 0) + (!root_path.empty() ? 1 : 0);
    if (modes != 1) {
        print_usage(argv[0]);
        return 2;
    }
    if (compile_shaders && root_path.empty()) {
        std::fprintf(stderr, "--compile-shaders requires --root\n");
        return 2;
    }

    if (!brf_path.empty()) return run_brf_file(brf_path, json);
    if (!dds_path.empty()) return run_dds_file(dds_path, json);
    if (!dir_path.empty()) return run_dir(dir_path, json);
    return run_module_catalog(root_path, module_name, json, compile_shaders, jobs);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run_cli(argc, argv);
    } catch (const std::bad_alloc&) {
        std::fprintf(stderr, "ERROR out of memory\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR %s\n", e.what());
    }
    return 1;
}
