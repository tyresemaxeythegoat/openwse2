// Clean-room Phase 2G Warband/WSE2 .brf / .dds asset inspection CLI.
//
// Modes:
//   wse_brf --brf <file.brf> [--json]
//   wse_brf --dds <file.dds> [--json]
//   wse_brf --dir <dir> [--json]
//   wse_brf --root <warband_root> [--module <name>] [--json]

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/game/module_loader_common.h"
#include "wse/platform/vfs.h"

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage:\n"
                 "  %s --brf <file.brf> [--json]\n"
                 "  %s --dds <file.dds> [--json]\n"
                 "  %s --dir <dir> [--json]\n"
                 "  %s --root <warband_root> [--module <name>] [--json]\n",
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
                       bool json) {
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
                    "\"physics_materials\":%zu}\n",
                    cat.entries.size(), loaded_entries, cat.warnings.size(),
                    totals.meshes, totals.meta_meshes, totals.textures, totals.shaders,
                    totals.materials, totals.bodies, totals.skeletons, totals.animations,
                    totals.physics_materials);
    } else {
        std::printf("CATALOG %s (%s): entries=%zu loaded=%zu warnings=%zu "
                    "(meshes=%zu meta=%zu tex=%zu shader=%zu mat=%zu body=%zu skel=%zu anim=%zu phys=%zu)\n",
                    root.string().c_str(), mod_rel.c_str(), cat.entries.size(), loaded_entries,
                    cat.warnings.size(), totals.meshes, totals.meta_meshes, totals.textures,
                    totals.shaders, totals.materials, totals.bodies, totals.skeletons,
                    totals.animations, totals.physics_materials);
    }
    return 0;
}

int run_cli(int argc, char** argv) {
    std::string brf_path;
    std::string dds_path;
    std::string dir_path;
    std::string root_path;
    std::string module_name = "Native";
    bool json = false;

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

    if (!brf_path.empty()) return run_brf_file(brf_path, json);
    if (!dds_path.empty()) return run_dds_file(dds_path, json);
    if (!dir_path.empty()) return run_dir(dir_path, json);
    return run_module_catalog(root_path, module_name, json);
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
