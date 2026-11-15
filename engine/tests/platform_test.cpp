// Host tests for the platform seam (Vfs, Clock, LogSink).
//
// Build/run: see AGENTS.md section 2 (CMake/CTest or the shell loop).

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#if __has_include("driver_loader.h")
#include "driver_loader.h"
#else
#include "../../platform/vulkan/driver_loader.h"
#endif

#include "wse/io/text_stream.h"
#include "wse/platform/clock.h"
#include "wse/platform/log.h"
#include "wse/platform/rhi.h"
#include "wse/platform/saf_vfs.h"
#include "wse/platform/sleep.h"
#include "wse/platform/vfs.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

void test_paths() {
    using namespace wse::platform;

    expect(normalize_path("Modules\\Native/Scripts.TXT") == "modules/native/scripts.txt",
           "both separators and case fold");
    expect(normalize_path("./a//b/./c/") == "a/b/c", "empty and dot components drop");
    expect(normalize_path("a/b/../c") == "a/c", "dot-dot stays inside the root");
    expect(normalize_path("") == "", "empty path is the root");
    expect(normalize_path("/") == "", "leading separator is the root");
    expect(!normalize_path("../secret"), "dot-dot above the root is rejected");
    expect(!normalize_path("a/../../b"), "late escape is rejected");
    expect(!normalize_path("C:\\Windows"), "drive letters are rejected");
    expect(!normalize_path(std::string_view("a\0b", 3)), "embedded NUL is rejected");
}

// Shared behaviour every Vfs must have. `vfs` starts with:
//   Modules/Native/scripts.txt = "scriptsfile version 1\n0\n"
//   Modules/Native/Resource/   (directory)
void check_contract(wse::platform::Vfs& vfs, const char* label) {
    using namespace wse::platform;
    std::printf("contract: %s\n", label);

    std::string text;
    expect(vfs.read_text("modules\\native\\SCRIPTS.txt", text), "case-insensitive read");
    expect(text == "scriptsfile version 1\n0\n", "read returns the file bytes");
    expect(vfs.is_file("Modules/Native/scripts.txt"), "is_file on a file");
    expect(!vfs.is_dir("Modules/Native/scripts.txt"), "a file is not a directory");
    expect(vfs.is_dir("MODULES/native"), "is_dir is case-insensitive");
    expect(vfs.is_dir(""), "the root is a directory");
    expect(!vfs.is_file("Modules/Native/missing.txt"), "missing file");
    expect(!vfs.read_text("Modules/Native/missing.txt", text), "read of a missing file fails");
    expect(!vfs.read_text("../outside.txt", text), "escape is refused");

    expect(vfs.write_text("Modules/Native/Resource/b.brf", "B"), "write a new file");
    expect(vfs.write_text("Modules/Native/Resource/A.brf", "A"), "write a second file");
    expect(vfs.write_text("modules/native/resource/a.BRF", "AA"), "rewrite through another case");

    std::vector<VfsEntry> entries;
    expect(vfs.list_dir("Modules/Native/Resource", entries), "list a directory");
    expect(entries.size() == 2, "rewrite did not create a second entry");
    if (entries.size() == 2) {
        expect(entries[0].name == "A.brf" && entries[1].name == "b.brf",
               "entries keep their spelling and sort case-insensitively");
        expect(!entries[0].is_directory, "files are not directories");
    }
    expect(vfs.read_text("Modules/Native/Resource/A.brf", text) && text == "AA",
           "rewrite replaced the contents");

    expect(vfs.list_dir("Modules/Native", entries), "list the module directory");
    expect(entries.size() == 2, "module directory has a directory and a file");
    if (entries.size() == 2) {
        expect(entries[0].name == "Resource" && entries[0].is_directory, "directory entry");
        expect(entries[1].name == "scripts.txt" && !entries[1].is_directory, "file entry");
    }
    expect(!vfs.list_dir("Modules/Nope", entries), "listing a missing directory fails");

    expect(vfs.remove_file("MODULES/NATIVE/RESOURCE/B.BRF"), "remove is case-insensitive");
    expect(!vfs.is_file("Modules/Native/Resource/b.brf"), "removed file is gone");
    expect(!vfs.remove_file("Modules/Native/Resource/b.brf"), "second remove fails");
    expect(!vfs.remove_file("Modules/Native/Resource"), "remove_file refuses a directory");
}

void test_memory_vfs() {
    wse::platform::MemoryVfs vfs;
    expect(vfs.write_text("Modules/Native/scripts.txt", "scriptsfile version 1\n0\n"),
           "memory write creates parents");
    expect(vfs.write_text("Modules/Native/Resource/seed", ""), "seed the resource directory");
    expect(vfs.remove_file("Modules/Native/Resource/seed"), "directories outlive their files");
    expect(!vfs.write_text("Modules/Native", "x"), "a directory cannot be overwritten");
    expect(!vfs.write_text("Modules/Native/scripts.txt/x", "x"), "a file is not a parent");
    check_contract(vfs, "MemoryVfs");
}

void test_host_vfs() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "wse_platform_test_vfs";
    fs::remove_all(root, ec);
    fs::create_directories(root / "Modules" / "Native" / "Resource", ec);
    expect(!ec, "host fixture directory created");

    wse::platform::HostVfs vfs(root);
    expect(vfs.write_text("Modules/Native/scripts.txt", "scriptsfile version 1\n0\n"),
           "host write into an existing directory");
    expect(!vfs.write_text("Modules/Missing/file.txt", "x"),
           "host write does not create parents");
    check_contract(vfs, "HostVfs");

    // A symbolic link inside the tree must not let a path leave the root.
    // Skipped where the host cannot create links (e.g. unprivileged Windows).
    const fs::path outside = fs::temp_directory_path(ec) / "wse_platform_test_outside";
    fs::remove_all(outside, ec);
    fs::create_directories(outside, ec);
    {
        std::FILE* secret = std::fopen((outside / "secret.txt").string().c_str(), "wb");
        if (secret != nullptr) {
            std::fputs("secret", secret);
            std::fclose(secret);
        }
    }
    std::error_code linkEc;
    fs::create_directory_symlink(outside, root / "Modules" / "escape", linkEc);
    if (!linkEc) {
        std::string leaked;
        expect(!vfs.read_text("Modules/escape/secret.txt", leaked) && leaked.empty(),
               "host read through an escaping symlink is refused");
        expect(!vfs.write_text("Modules/escape/new.txt", "x") &&
                   !fs::exists(outside / "new.txt", ec),
               "host write through an escaping symlink is refused");
        expect(!vfs.is_file("Modules/escape/secret.txt") && !vfs.is_dir("Modules/escape"),
               "escaping symlink is not reported as a file or directory");
        expect(!vfs.remove_file("Modules/escape/secret.txt") &&
                   fs::exists(outside / "secret.txt", ec),
               "host remove through an escaping symlink is refused");
    }
    fs::remove_all(outside, ec);
    fs::remove_all(root, ec);

    // A root that does not exist yet is created by the first write, not by
    // construction or by a read.
    const fs::path fresh = fs::temp_directory_path(ec) / "wse_platform_test_fresh" / "saves";
    fs::remove_all(fresh.parent_path(), ec);
    {
        wse::platform::HostVfs fresh_vfs(fresh);
        std::string text;
        expect(!fresh_vfs.read_text("a.txt", text) && !fs::exists(fresh, ec),
               "host read does not create a missing root");
        expect(fresh_vfs.write_text("a.txt", "first") && fs::exists(fresh / "a.txt", ec),
               "host write creates a missing root");
        expect(fresh_vfs.read_text("A.TXT", text) && text == "first",
               "host read_text fills a string directly");
        // Names differing only in case are one file, as on Windows; the
        // spelling on disk stays that of the first write.
        expect(fresh_vfs.write_text("A.txt", "second") && fresh_vfs.read_text("a.txt", text) &&
                   text == "second",
               "host write to a differently-cased name overwrites the same file");
        std::vector<wse::platform::VfsEntry> entries;
        expect(fresh_vfs.list_dir("", entries) && entries.size() == 1 &&
                   entries[0].name == "a.txt",
               "case-colliding writes leave a single file");
        expect(!fresh_vfs.write_text("sub/b.txt", "x"),
               "host write still does not create parents below the root");
    }
    fs::remove_all(fresh.parent_path(), ec);
}

// HostVfs keeps one lower-case index per directory for case-insensitive
// lookups; misses must not rescan, and changes must be seen.
void test_host_vfs_dir_cache() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "wse_platform_test_dircache";
    fs::remove_all(root, ec);
    fs::create_directories(root / "Textures", ec);
    expect(!ec, "dir-cache fixture directory created");
    const auto touch = [](const fs::path& p) {
        std::FILE* f = std::fopen(p.string().c_str(), "wb");
        if (f != nullptr) std::fclose(f);
    };
    for (int i = 0; i < 50; ++i) touch(root / "Textures" / ("tex_" + std::to_string(i) + ".dds"));
    touch(root / "Textures" / "Mixed_Case.DDS");

    wse::platform::HostVfs vfs(root);
    expect(vfs.is_file("textures/mixed_case.dds"), "case-insensitive hit through the index");
    const std::size_t after_first = vfs.directory_scans();
    for (int i = 0; i < 20; ++i) {
        const std::string dir_case = (i % 2 == 0) ? "Textures" : "textures";
        (void)vfs.is_file(dir_case + "/languages/en/missing_" + std::to_string(i) + ".dds");
        (void)vfs.is_file("TEXTURES/MIXED_CASE.dds");
    }
    // On case-sensitive filesystems, root and Textures are listed once each (2);
    // on case-insensitive filesystems, the first miss under `Textures` lists it once (1)
    // and `textures/...` misses reuse that case-folded cache entry.
    expect(vfs.directory_scans() >= 1U && vfs.directory_scans() <= 2U &&
               vfs.directory_scans() >= after_first,
           "repeated misses reuse the cached directory listing");

    // A file written through the Vfs is visible under another spelling at once.
    expect(vfs.write_text("Textures/New_File.txt", "x") && vfs.is_file("textures/NEW_FILE.TXT"),
           "write through the Vfs invalidates the cached listing");
    expect(vfs.remove_file("textures/new_file.txt") && !vfs.is_file("Textures/New_File.txt"),
           "remove through the Vfs invalidates the cached listing");
    expect(vfs.make_dir("Textures/Sub") && vfs.is_dir("textures/SUB"),
           "make_dir through the Vfs invalidates the cached listing");

    // An entry added by another process is seen once the directory's
    // last-write time moves (set explicitly so coarse timestamps cannot hide it).
    expect(!vfs.is_file("textures/external.dds"), "external file absent before creation");
    const auto before = fs::last_write_time(root / "Textures", ec);
    touch(root / "Textures" / "External.DDS");
    fs::last_write_time(root / "Textures", before + std::chrono::seconds(2), ec);
    expect(!ec && vfs.is_file("textures/external.dds"),
           "external change is picked up when the directory time moves");

    fs::remove_all(root, ec);
}

void test_vfs_feeds_text_stream() {
    wse::platform::MemoryVfs vfs;
    expect(vfs.write_text("Modules/Native/triggers.txt", "triggersfile version 1\n 7 \n"),
           "write module text");
    std::string text;
    expect(vfs.read_text("modules/native/TRIGGERS.TXT", text), "read module text");
    wse::io::TokenReader reader(text);
    expect(reader.next_token() == "triggersfile", "first header token");
    expect(reader.skip_tokens(2), "version tokens");
    std::int32_t count = 0;
    expect(reader.read_i32(count) && count == 7, "count token parses");
    expect(reader.at_end(), "all tokens consumed");
}

void test_clock() {
    using namespace wse::platform;
    ManualClock manual;
    expect(manual.ticks() == 0, "manual clock starts at zero");
    manual.advance_seconds(1.5);
    expect(manual.ticks() == 150000, "100,000 ticks per second");
    manual.advance_ticks(50000);
    expect(manual.seconds() == 2.0, "seconds derive from ticks");

    SteadyClock steady;
    const std::int64_t first = steady.ticks();
    const std::int64_t second = steady.ticks();
    expect(first >= 0 && second >= first, "steady clock is monotonic");

    ManualWallClock manual_wall;
    manual_wall.set(1'700'000'000, -18000);
    expect(manual_wall.unix_seconds() == 1'700'000'000 &&
               manual_wall.utc_offset_seconds(0) == -18000,
           "manual wall clock returns what was set");
    SystemWallClock system_wall;
    const std::int64_t now = system_wall.unix_seconds();
    const std::int32_t offset = system_wall.utc_offset_seconds(now);
    expect(now > 1'600'000'000, "system wall clock is past 2020");
    expect(offset > -24 * 3600 && offset < 24 * 3600 && offset % 60 == 0,
           "system UTC offset is a whole number of minutes within a day");

    sleep_ms(-5);
    sleep_ms(0);
    sleep_ms(1);
}

void test_log() {
    using namespace wse::platform;
    CaptureLog log;
    LogSink& sink = log;
    sink.warning("Unable to map script x");
    sink.error("boom");
    expect(log.lines.size() == 2, "captured two lines");
    expect(log.lines[0].level == LogLevel::warning &&
               log.lines[0].message == "Unable to map script x",
           "warning captured");
    expect(std::string(log_level_name(log.lines[1].level)) == "error", "level names");
    NullLog null;
    null.info("dropped");
}

void test_saf_vfs() {
    using namespace wse::platform;

    // 1. Contract compliance over MemorySafBackend.
    {
        auto backend = std::make_shared<MemorySafBackend>();
        std::string doc_modules;
        std::string doc_native;
        std::string doc_resource;
        std::string doc_scripts;
        expect(backend->create_directory(backend->root_document_id(), "Modules", doc_modules),
               "saf seed Modules");
        expect(backend->create_directory(doc_modules, "Native", doc_native),
               "saf seed Modules/Native");
        expect(backend->create_directory(doc_native, "Resource", doc_resource),
               "saf seed Modules/Native/Resource");
        const std::string scripts_seed = "scriptsfile version 1\n0\n";
        expect(backend->write_document(
                   doc_native, {}, "scripts.txt",
                   std::span<const std::uint8_t>(
                       reinterpret_cast<const std::uint8_t*>(scripts_seed.data()),
                       scripts_seed.size()),
                   doc_scripts),
               "saf seed scripts.txt");

        SafVfs saf(backend);
        expect(saf.index_complete(), "saf initial index succeeds");
        backend->reset_call_counts();
        check_contract(saf, "SafVfs");
        expect(backend->list_children_calls == 0,
               "SafVfs contract ops never re-query list_children");
    }

    // 2. One-time index, write/mkdir/rmdir updates, module scanner, scripts check, and import_to.
    {
        MemoryVfs seed_vfs;
        const std::string native_ini =
            "# Native config\n"
            "module_name = Calradia_Native\n"
            "compatible_with_warband = 1\n"
            "load_resource = core_shaders\n"
            "load_resource = core_textures\n"
            "load_mod_resource = custom_ui\n";
        const std::string native_scripts =
            "scriptsfile version 1\n"
            "4\n"
            "game_start -1\n"
            " 2\n"
            " 2133 2 1224979098644774912 5\n"
            " 2105 2 1224979098644774912 1\n"
            "unmapped_custom_script 0.5\n"
            " 1\n"
            " 1 1 7\n"
            "game_start 2.25\n"
            " 1\n"
            " 2320 2 1224979098644774913 1\n"
            "last_script -1\n"
            " 0\n";
        const std::string custom_ini =
            "module_name = Prophesy_Sample\n"
            "load_resource = test_res\n"
            "load_mod_resource = custom_a\n"
            "load_mod_resource = custom_b\n";

        expect(seed_vfs.write_text("Modules/CustomMod/module.ini", custom_ini), "seed custom mod");
        expect(seed_vfs.write_text("Modules/Native/module.ini", native_ini), "seed native ini");
        expect(seed_vfs.write_text("Modules/Native/scripts.txt", native_scripts),
               "seed native scripts");
        expect(seed_vfs.write_text("CommonRes/core_shaders.brf", "BRF0"), "seed commonres");
        expect(seed_vfs.make_dir("Modules/EmptyFolder"), "seed empty folder without ini");

        auto backend = std::make_shared<MemorySafBackend>();
        expect(backend->seed_from_vfs(seed_vfs), "seed MemorySafBackend from Vfs");

        SafVfs saf(backend);
        expect(saf.index_complete(), "saf index complete");
        expect(saf.index_file_count() == 4, "saf indexed 4 files");
        expect(saf.index_dir_count() == 5, "saf indexed 5 dirs");
        backend->reset_call_counts();

        // Module discovery should find Native first and skip EmptyFolder (no module.ini).
        const auto modules = scan_warband_modules(saf);
        expect(modules.size() == 2, "scan_warband_modules finds 2 modules");
        if (modules.size() == 2) {
            expect(modules[0].folder_name == "Native", "Native module sorted first");
            expect(modules[0].display_name == "Calradia Native", "Native display_name cleaned");
            expect(modules[0].has_scripts_txt, "Native has scripts.txt");
            expect(modules[0].load_resource_count == 2 && modules[0].load_mod_resource_count == 1,
                   "Native resource counts");
            expect(modules[0].compatible_with_warband == "1", "Native compatibility flag");
            expect(modules[1].folder_name == "CustomMod", "CustomMod sorted second");
            expect(modules[1].display_name == "Prophesy Sample", "CustomMod display_name cleaned");
            expect(!modules[1].has_scripts_txt, "CustomMod lacks scripts.txt");
            expect(modules[1].load_resource_count == 1 && modules[1].load_mod_resource_count == 2,
                   "CustomMod resource counts");
        }

        // Script verification through SafVfs.
        const ModuleScriptsCheckResult check = verify_module_scripts(saf, "Modules\\Native");
        expect(check.ok, "verify_module_scripts succeeds through SafVfs");
        expect(check.script_count == 4, "4 scripts parsed through SafVfs");
        expect(check.statement_count == 4, "4 statements parsed through SafVfs");
        expect(check.mapped_script_count == 1, "1 mapped script resolved through SafVfs");
        expect(!verify_module_scripts(saf, "Modules/CustomMod").ok,
               "verify_module_scripts fails cleanly when scripts.txt missing");

        // Write, mkdir, rmdir updates index in place without calling list_children.
        expect(saf.make_dir("Savegames/Native"), "SafVfs make_dir creates missing ancestors");
        expect(!saf.make_dir("Savegames/Native"), "SafVfs make_dir rejects existing dir");
        expect(saf.write_text("Savegames/Native/sg00.sav", "SAV"), "SafVfs write_text");
        expect(!saf.remove_dir("Savegames/Native"), "SafVfs remove_dir refuses non-empty dir");
        expect(saf.remove_file("savegames/native/SG00.SAV"), "SafVfs remove_file case-insensitive");
        expect(saf.remove_dir("Savegames/Native"), "SafVfs remove_dir succeeds when empty");
        expect(!saf.is_dir("Savegames/Native") && saf.is_dir("Savegames"),
               "Savegames parent remains after child removal");
        expect(backend->list_children_calls == 0,
               "no list_children binder calls during read/scan/write/mkdir/rmdir");

        // Import subtree to destination Vfs (app storage simulation).
        MemoryVfs imported;
        std::size_t files_copied = 0;
        std::uint64_t bytes_copied = 0;
        expect(saf.import_to(imported, "Modules/Native", &files_copied, &bytes_copied),
               "SafVfs import_to succeeds");
        expect(files_copied == 2, "import_to copied 2 Native files");
        expect(bytes_copied == native_ini.size() + native_scripts.size(),
               "import_to copied exact byte count");
        std::string imported_scripts;
        expect(imported.read_text("Modules/Native/scripts.txt", imported_scripts) &&
                   imported_scripts == native_scripts,
               "imported scripts.txt matches source");
        expect(!imported.is_file("CommonRes/core_shaders.brf"),
               "subtree import_to excluded CommonRes");
    }
}

void test_rhi_and_driver_loader() {
    using namespace wse::platform;

    // 1. AdrenoTools meta.json parsing (`Banners-Turnip` and `FristOneRR-Panvk-Driver`).
    {
        const std::string turnip_json = R"({
            "schemaVersion": 1,
            "name": "Mesa Turnip Driver v27.0.0",
            "description": "Custom Vulkan ICD for Adreno",
            "author": "The412Banner",
            "packageVersion": "1",
            "vendor": "Mesa",
            "driverVersion": "Vulkan 1.4.300",
            "minApi": 28,
            "libraryName": "libvulkan_freedreno.so"
        })";
        DriverPackageMeta meta{};
        std::string err;
        expect(parse_driver_meta_json(turnip_json, meta, &err), "parse Turnip meta.json");
        expect(meta.schema_version == 1 && meta.min_api == 28 &&
                   meta.library_name == "libvulkan_freedreno.so" &&
                   meta.inferred_mode == DriverMode::turnip,
               "Turnip metadata fields and mode inference");

        const std::string panvk_json = R"({
            "schemaVersion": 1,
            "name": "PanVK FristOneRR v3.7.0",
            "description": "PanVK ICD for Mali kbase",
            "author": "FristOneRR",
            "packageVersion": "1",
            "vendor": "Mesa",
            "driverVersion": "Vulkan 1.3.295",
            "minApi": 29,
            "libraryName": "libvulkan_panfrost.so"
        })";
        expect(parse_driver_meta_json(panvk_json, meta, &err), "parse PanVK meta.json");
        expect(meta.schema_version == 1 && meta.min_api == 29 &&
                   meta.library_name == "libvulkan_panfrost.so" &&
                   meta.inferred_mode == DriverMode::panvk,
               "PanVK metadata fields and mode inference");

        // Unsafe / invalid payloads rejected.
        expect(!parse_driver_meta_json(
                   R"({"schemaVersion":1,"libraryName":"../evil.so"})", meta, &err),
               "reject parent-traversal libraryName");
        expect(!parse_driver_meta_json(
                   R"({"schemaVersion":1,"libraryName":"sub/lib.so"})", meta, &err),
               "reject slash in libraryName");
        expect(!parse_driver_meta_json(
                   R"({"schemaVersion":1,"libraryName":"driver.txt"})", meta, &err),
               "reject non-.so/.dll libraryName");
        expect(!parse_driver_meta_json(
                   R"({"schemaVersion":2,"libraryName":"libvulkan_freedreno.so"})", meta, &err),
               "reject unsupported schemaVersion");
        expect(!parse_driver_meta_json("{not json", meta, &err), "reject malformed meta.json");

        // 2. ELF .dynstr `libhardware.so\0` -> `libpanvk_hw.so\0` patching and VFS staging.
        const std::string raw_dynstr(
            "\0libc.so\0libhardware.so\0liblog.so\0libhardware.so\0libm.so\0", 53);
        const std::string expected_dynstr(
            "\0libc.so\0libpanvk_hw.so\0liblog.so\0libpanvk_hw.so\0libm.so\0", 53);
        std::vector<std::uint8_t> elf_bytes(raw_dynstr.begin(), raw_dynstr.end());

        MemoryVfs stage_vfs;
        std::string staged_path;
        std::size_t patched_refs = 0;
        expect(stage_driver_package_to_vfs(stage_vfs, "drivers/panvk", panvk_json, elf_bytes,
                                           meta, staged_path, &patched_refs, &err),
               "stage PanVK driver package into Vfs");
        expect(patched_refs == 2U, "two libhardware.so DT_NEEDED references rewritten");
        expect(staged_path == "drivers/panvk/libvulkan_panfrost.so", "staged ICD path matches");
        std::vector<std::uint8_t> staged_elf;
        expect(stage_vfs.read_file(staged_path, staged_elf) &&
                   staged_elf.size() == expected_dynstr.size() &&
                   std::memcmp(staged_elf.data(), expected_dynstr.data(), expected_dynstr.size()) == 0,
               "staged ICD rewrote libhardware.so in place preserving ELF offsets");
    }

    // 3. VulkanDriverLoader + VulkanRhiDevice against Mock ICD v5 (PanVK and Turnip).
    {
        auto panvk_loader = std::make_shared<VulkanDriverLoader>();
        VulkanMockConfig panvk_cfg{};
        panvk_cfg.mode = DriverMode::panvk;
        panvk_cfg.device_name = "Mali-G610 (PanVK Mock ICD)";
        panvk_cfg.vendor_id = 0x13B5U;
        panvk_cfg.device_id = 0x0610U;
        panvk_cfg.direct_icd = true;
        expect(panvk_loader->load_mock_icd(panvk_cfg), "load mock PanVK ICD");

        VulkanDriverProbeResult probe{};
        expect(panvk_loader->probe_and_create_device(probe) && probe.ok &&
                   probe.used_direct_icd && probe.negotiated_icd_version == 5U &&
                   probe.mode == DriverMode::panvk &&
                   probe.device_name == "Mali-G610 (PanVK Mock ICD)",
               "PanVK direct ICD v5 handshake and device creation succeed");

        {
            VulkanRhiDevice vk_rhi(panvk_loader);
            expect(vk_rhi.backend_name() == "vulkan" &&
                       vk_rhi.driver_mode() == DriverMode::panvk &&
                       vk_rhi.device_name() == "Mali-G610 (PanVK Mock ICD)",
                   "VulkanRhiDevice reports probed PanVK device identity");
            const std::array<std::uint8_t, 16> buf_data{1, 2, 3, 4};
            const BufferHandle bh = vk_rhi.create_buffer(BufferUsage::vertex, buf_data);
            const std::array<std::uint8_t, 16> tex_data{
                255, 0, 0, 255,  0, 255, 0, 255,  0, 0, 255, 255,  255, 255, 255, 255};
            const TextureHandle th = vk_rhi.create_texture_rgba8(2U, 2U, tex_data);
            expect(bh != 0 && th != 0 && vk_rhi.live_vk_buffers() == 1U &&
                       vk_rhi.live_vk_images() == 1U,
                   "VulkanRhiDevice creates live VkBuffer and VkImage handles");
            vk_rhi.destroy_texture(th);
            vk_rhi.destroy_buffer(bh);
            expect(vk_rhi.live_vk_buffers() == 0U && vk_rhi.live_vk_images() == 0U,
                   "VulkanRhiDevice destroys live VkBuffer and VkImage handles");
        }

        // System Vulkan loader probe executes cleanly whether a host GPU is present or not.
        VulkanDriverLoader sys_loader;
        std::string sys_err;
        if (sys_loader.load_system(&sys_err)) {
            VulkanDriverProbeResult sys_probe{};
            (void)sys_loader.probe_and_create_device(sys_probe);
        } else {
            expect(!sys_err.empty(), "missing system Vulkan loader yields diagnostic");
        }
    }

    // 4. P4 textured static mesh viewer screenshot test (SoftwareRhiDevice vs VulkanRhiDevice).
    {
        auto build_bc1_dds = [](std::uint16_t color0_565, std::uint16_t color1_565,
                                std::uint32_t indices_u32) {
            std::vector<std::byte> buf(136U, std::byte{0});
            const auto w16 = [&](std::size_t off, std::uint16_t v) {
                buf[off + 0U] = static_cast<std::byte>(v & 0xFFU);
                buf[off + 1U] = static_cast<std::byte>((v >> 8U) & 0xFFU);
            };
            const auto w32 = [&](std::size_t off, std::uint32_t v) {
                buf[off + 0U] = static_cast<std::byte>(v & 0xFFU);
                buf[off + 1U] = static_cast<std::byte>((v >> 8U) & 0xFFU);
                buf[off + 2U] = static_cast<std::byte>((v >> 16U) & 0xFFU);
                buf[off + 3U] = static_cast<std::byte>((v >> 24U) & 0xFFU);
            };
            buf[0] = std::byte{'D'};
            buf[1] = std::byte{'D'};
            buf[2] = std::byte{'S'};
            buf[3] = std::byte{' '};
            w32(4U, 124U);
            w32(8U, 0x1007U);
            w32(12U, 4U);
            w32(16U, 4U);
            w32(20U, 8U);
            w32(76U, 32U);
            w32(80U, 0x4U);
            buf[84] = std::byte{'D'};
            buf[85] = std::byte{'X'};
            buf[86] = std::byte{'T'};
            buf[87] = std::byte{'1'};
            w32(108U, 0x1000U);
            w16(128U, color0_565);
            w16(130U, color1_565);
            w32(132U, indices_u32);
            return buf;
        };

        // Solid red BC1 4x4 texture (color0 = 0xF800 -> RGBA(255, 0, 0, 255), all indices = 0).
        const auto dds_bytes = build_bc1_dds(0xF800U, 0x07E0U, 0x00000000U);
        wse::asset::DdsImageView dds{};
        std::string err;
        expect(wse::asset::parse_dds(dds_bytes, dds, &err), "parse synthetic BC1 DDS texture");

        wse::asset::BrfMesh mesh{};
        mesh.name = "p4_viewer_quad";
        mesh.positions = {
            {-0.6f, -0.6f, 0.0f},
            {0.6f, -0.6f, 0.0f},
            {0.6f, 0.6f, 0.0f},
            {-0.6f, 0.6f, 0.0f},
        };
        const std::array<wse::asset::Vec2f, 4> uvs{
            wse::asset::Vec2f{0.0f, 1.0f},
            wse::asset::Vec2f{1.0f, 1.0f},
            wse::asset::Vec2f{1.0f, 0.0f},
            wse::asset::Vec2f{0.0f, 0.0f},
        };
        for (std::size_t i = 0; i < 4U; ++i) {
            wse::asset::BrfFaceCorner c{};
            c.vertex_index = static_cast<std::int32_t>(i);
            c.color = 0xFFFFFFFFU;
            c.normal = {0.0f, 0.0f, 1.0f};
            c.uv = uvs[i];
            c.uv2 = uvs[i];
            mesh.face_corners.push_back(c);
        }
        mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 1, 2}});
        mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 2, 3}});

        float mvp[16]{};
        make_orbit_mvp_matrix(1.0f, 1.8f, 0.0f, 0.0f, mvp);

        SoftwareRhiDevice sw_rhi;
        std::vector<std::uint8_t> sw_frame;
        expect(render_textured_brf_mesh_frame(sw_rhi, mesh, dds, 64U, 64U, mvp, sw_frame, &err),
               "render textured BrfMesh through SoftwareRhiDevice");

        auto vk_loader = std::make_shared<VulkanDriverLoader>();
        VulkanMockConfig turnip_cfg{};
        turnip_cfg.mode = DriverMode::turnip;
        turnip_cfg.device_name = "Turnip Adreno (TM) 740 (Mock ICD)";
        turnip_cfg.vendor_id = 0x5143U;
        turnip_cfg.device_id = 0x0740U;
        VulkanDriverProbeResult turnip_probe{};
        expect(vk_loader->load_mock_icd(turnip_cfg) &&
                   vk_loader->probe_and_create_device(turnip_probe),
               "initialize Turnip mock ICD for RHI screenshot test");
        VulkanRhiDevice vk_rhi(vk_loader);
        std::vector<std::uint8_t> vk_frame;
        expect(render_textured_brf_mesh_frame(vk_rhi, mesh, dds, 64U, 64U, mvp, vk_frame, &err),
               "render textured BrfMesh through VulkanRhiDevice");

        expect(sw_frame.size() == 64U * 64U * 4U && vk_frame.size() == sw_frame.size(),
               "rendered frame dimensions match 64x64 RGBA8");
        const std::size_t center_off = (32U * 64U + 32U) * 4U;
        expect(vk_frame[0] == 18U && vk_frame[1] == 22U && vk_frame[2] == 30U && vk_frame[3] == 255U,
               "corner pixel preserves render pass clear color");
        expect(vk_frame[center_off + 0U] == 255U && vk_frame[center_off + 1U] == 0U &&
                   vk_frame[center_off + 2U] == 0U && vk_frame[center_off + 3U] == 255U,
               "center pixel shows decoded red BC1 texture modulated by white corner color");
        const std::uint64_t sw_hash = hash_rgba8_frame(sw_frame);
        const std::uint64_t vk_hash = hash_rgba8_frame(vk_frame);
        expect(sw_hash != 0ULL && sw_hash == vk_hash,
               "SoftwareRhiDevice and VulkanRhiDevice produce identical deterministic frame hash");

        // Smoke helper succeeds across System / Turnip / PanVK modes.
        std::uint64_t sys_smoke_hash = 0;
        std::uint64_t turnip_smoke_hash = 0;
        std::uint64_t panvk_smoke_hash = 0;
        expect(run_vulkan_mesh_smoke_test(DriverMode::system, "", "", true, sys_smoke_hash).ok &&
                   run_vulkan_mesh_smoke_test(DriverMode::turnip, "", "", true, turnip_smoke_hash).ok &&
                   run_vulkan_mesh_smoke_test(DriverMode::panvk, "", "", true, panvk_smoke_hash).ok &&
                   sys_smoke_hash != 0ULL && sys_smoke_hash == turnip_smoke_hash &&
                   turnip_smoke_hash == panvk_smoke_hash,
               "Vulkan mesh smoke test passes consistently across System, Turnip, and PanVK");

        // Depth buffer occlusion test (near green z=0.3 occludes far red z=0.7 in either order).
        const std::array<std::uint8_t, 4> white_tex{255, 255, 255, 255};
        const TextureHandle white_th = sw_rhi.create_texture_rgba8(1U, 1U, white_tex);
        const PipelineHandle pip = sw_rhi.create_pipeline(PipelineDesc{true, true, CullMode::back});
        const std::array<MeshVertex, 3> far_verts{{
            {-0.5f, 0.5f, 0.7f, 0, 0, 1, 0, 0, argb_to_rgba8(0xFFFF0000U)},
            {0.5f, 0.5f, 0.7f, 0, 0, 1, 1, 0, argb_to_rgba8(0xFFFF0000U)},
            {0.0f, -0.5f, 0.7f, 0, 0, 1, 0.5f, 1, argb_to_rgba8(0xFFFF0000U)},
        }};
        const std::array<MeshVertex, 3> near_verts{{
            {-0.5f, 0.5f, 0.3f, 0, 0, 1, 0, 0, argb_to_rgba8(0xFF00FF00U)},
            {0.5f, 0.5f, 0.3f, 0, 0, 1, 1, 0, argb_to_rgba8(0xFF00FF00U)},
            {0.0f, -0.5f, 0.3f, 0, 0, 1, 0.5f, 1, argb_to_rgba8(0xFF00FF00U)},
        }};
        const std::array<std::uint16_t, 3> tri_idx{0, 1, 2};
        const auto as_u8_span = [](const void* ptr, std::size_t bytes) noexcept {
            return std::span<const std::uint8_t>(
                static_cast<const std::uint8_t*>(ptr), bytes);
        };
        const BufferHandle far_vb =
            sw_rhi.create_buffer(BufferUsage::vertex, as_u8_span(far_verts.data(), sizeof(far_verts)));
        const BufferHandle near_vb = sw_rhi.create_buffer(
            BufferUsage::vertex, as_u8_span(near_verts.data(), sizeof(near_verts)));
        const BufferHandle ib =
            sw_rhi.create_buffer(BufferUsage::index, as_u8_span(tri_idx.data(), sizeof(tri_idx)));

        MeshDrawCall far_draw{};
        far_draw.pipeline = pip;
        far_draw.vertex_buffer = far_vb;
        far_draw.index_buffer = ib;
        far_draw.texture = white_th;
        far_draw.index_count = 3U;

        MeshDrawCall near_draw = far_draw;
        near_draw.vertex_buffer = near_vb;

        RenderPassDesc pass{};
        pass.width = 64U;
        pass.height = 64U;

        std::vector<std::uint8_t> occ_a;
        std::vector<std::uint8_t> occ_b;
        const std::array<MeshDrawCall, 2> order_far_then_near{far_draw, near_draw};
        const std::array<MeshDrawCall, 2> order_near_then_far{near_draw, far_draw};
        expect(sw_rhi.render_pass_and_readback(pass, order_far_then_near, occ_a, &err) &&
                   sw_rhi.render_pass_and_readback(pass, order_near_then_far, occ_b, &err),
               "depth occlusion passes succeed");
        expect(occ_a[center_off + 0U] == 0U && occ_a[center_off + 1U] == 255U &&
                   occ_a[center_off + 2U] == 0U && occ_a[center_off + 3U] == 255U &&
                   occ_a == occ_b,
               "nearer triangle occludes farther triangle regardless of draw order");

        sw_rhi.destroy_buffer(ib);
        sw_rhi.destroy_buffer(near_vb);
        sw_rhi.destroy_buffer(far_vb);
        sw_rhi.destroy_pipeline(pip);
        sw_rhi.destroy_texture(white_th);
    }
}

}  // namespace

namespace wse::selftest {

int run_platform_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_paths();
    test_memory_vfs();
    test_host_vfs();
    test_host_vfs_dir_cache();
    test_vfs_feeds_text_stream();
    test_clock();
    test_log();
    test_saf_vfs();
    test_rhi_and_driver_loader();
    if (failures != 0) {
        std::printf("platform tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("platform tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_platform_suite(nullptr);
}
#endif
