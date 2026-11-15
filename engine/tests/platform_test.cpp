// Host tests for the platform seam (Vfs, Clock, LogSink).
//
// Build/run: see AGENTS.md section 2 (CMake/CTest or the shell loop).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "wse/io/text_stream.h"
#include "wse/platform/clock.h"
#include "wse/platform/log.h"
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
        (void)vfs.is_file("Textures/languages/en/missing_" + std::to_string(i) + ".dds");
        (void)vfs.is_file("TEXTURES/MIXED_CASE.dds");
    }
    // At most the root and Textures listed once each across all lookups.
    expect(after_first >= 1U && vfs.directory_scans() <= 2U,
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
