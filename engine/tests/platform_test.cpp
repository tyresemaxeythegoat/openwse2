// Host tests for the platform seam (Vfs, Clock, LogSink).
//
// Build/run: see AGENTS.md section 2 (CMake/CTest or the shell loop).

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
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

#include "wse/asset/shader.h"
#include "wse/asset/shader_compiler.h"
#include "wse/io/text_stream.h"
#include "wse/platform/clock.h"
#include "wse/platform/log.h"
#include "wse/platform/driver_package.h"
#include "wse/platform/input.h"
#include "wse/platform/performance.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_brf.h"
#include "wse/platform/rhi_material.h"
#include "wse/platform/saf_vfs.h"
#include "wse/platform/sleep.h"
#include "wse/platform/vfs.h"

namespace {

// The real host Vulkan loader and whatever ICDs it finds (GPU drivers, lavapipe)
// make results machine-dependent, and drivers leak by design, which fails
// LeakSanitizer. Only exercise them when asked: WSE_TEST_SYSTEM_VULKAN=1.
bool system_vulkan_tests_enabled() {
    const char* v = std::getenv("WSE_TEST_SYSTEM_VULKAN");
    return v != nullptr && v[0] == '1';
}

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
    // Case-sensitive volume: the first lookup lists root and Textures (2) and the
    // loop lists nothing new. Case-insensitive volume: the first lookup needs no
    // listing and the loop lists `Textures` once; `textures/...` misses reuse
    // that case-folded entry. Either way the loop adds at most one listing.
    expect(after_first <= 2U && vfs.directory_scans() - after_first <= 1U &&
               vfs.directory_scans() <= 2U,
           "repeated misses reuse the cached directory listing");

    // Two directories that differ only in case (case-sensitive volume only)
    // share a folded cache key but must not share a listing.
    {
        std::error_code pack_ec;
        std::filesystem::create_directories(root / "Pack", pack_ec);
        std::filesystem::create_directory(root / "pack", pack_ec);
        const bool distinct = !pack_ec && !std::filesystem::equivalent(root / "Pack", root / "pack", pack_ec);
        if (distinct) {
            touch(root / "Pack" / "Alpha.txt");
            touch(root / "pack" / "Beta.txt");
            // Equal timestamps, so only the spelling check can tell the listings apart.
            const auto stamp = std::filesystem::last_write_time(root / "Pack", pack_ec);
            std::filesystem::last_write_time(root / "pack", stamp, pack_ec);
            expect(vfs.is_file("Pack/alpha.TXT") && vfs.is_file("pack/beta.TXT") &&
                       vfs.is_file("Pack/ALPHA.txt") && !vfs.is_file("Pack/beta.txt"),
                   "case-folded cache key keeps case-distinct directories apart");
        }
    }

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

// Minimal little-endian ELF64 shared object: one PT_LOAD covering the file
// (vaddr == file offset), a PT_DYNAMIC with DT_NEEDED entries, DT_STRTAB/DT_STRSZ,
// and one DT_VERNEED record whose vn_file names a *separate* copy of
// "libhardware.so". A `.rodata`-style decoy copy follows the string table and
// must never be patched.
struct TestElf {
    std::vector<std::uint8_t> bytes;
    std::size_t needed_name_off = 0;   // file offset of DT_NEEDED "libhardware.so"
    std::size_t verneed_name_off = 0;  // file offset of vn_file "libhardware.so"
    std::size_t decoy_off = 0;         // file offset of the unreferenced decoy
};

TestElf build_test_elf64() {
    TestElf elf;
    std::vector<std::uint8_t>& b = elf.bytes;
    const auto put = [&b](std::size_t off, std::uint64_t v, unsigned width) {
        if (b.size() < off + width) b.resize(off + width, 0);
        for (unsigned i = 0; i < width; ++i) b[off + i] = static_cast<std::uint8_t>(v >> (8U * i));
    };
    const std::size_t phoff = 64, strtab = 64 + 2 * 56;
    const std::string strings("\0libc.so\0libhardware.so\0liblog.so\0libhardware.so\0", 50);
    const std::size_t s_libc = 1, s_hw = 9, s_log = 24, s_hw2 = 34;
    b.assign(strtab, 0);
    b.insert(b.end(), strings.begin(), strings.end());
    elf.needed_name_off = strtab + s_hw;
    elf.verneed_name_off = strtab + s_hw2;
    const std::string decoy("dlopen(libhardware.so)\0libhardware.so\0", 38);
    elf.decoy_off = b.size() + 23;
    b.insert(b.end(), decoy.begin(), decoy.end());
    while (b.size() % 8 != 0) b.push_back(0);
    const std::size_t verneed = b.size();
    put(verneed + 0, 1, 2);       // vn_version
    put(verneed + 2, 0, 2);       // vn_cnt
    put(verneed + 4, s_hw2, 4);   // vn_file
    put(verneed + 8, 0, 4);       // vn_aux
    put(verneed + 12, 0, 4);      // vn_next
    const std::size_t dyn = verneed + 16;
    const std::uint64_t entries[][2] = {
        {1, s_libc}, {1, s_hw}, {1, s_log}, {5, strtab}, {10, strings.size()},
        {0x6ffffffeULL, verneed}, {0x6fffffffULL, 1}, {0, 0}};
    std::size_t pos = dyn;
    for (const auto& e : entries) {
        put(pos, e[0], 8);
        put(pos + 8, e[1], 8);
        pos += 16;
    }
    const std::size_t total = pos;
    b[0] = 0x7F; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
    put(16, 3, 2);         // e_type = ET_DYN
    put(18, 183, 2);       // e_machine = EM_AARCH64
    put(20, 1, 4);         // e_version
    put(32, phoff, 8);     // e_phoff
    put(52, 64, 2);        // e_ehsize
    put(54, 56, 2);        // e_phentsize
    put(56, 2, 2);         // e_phnum
    // PT_LOAD over the whole file.
    put(phoff + 0, 1, 4);
    put(phoff + 8, 0, 8);
    put(phoff + 16, 0, 8);
    put(phoff + 32, total, 8);
    put(phoff + 40, total, 8);
    // PT_DYNAMIC.
    put(phoff + 56 + 0, 2, 4);
    put(phoff + 56 + 8, dyn, 8);
    put(phoff + 56 + 16, dyn, 8);
    put(phoff + 56 + 32, total - dyn, 8);
    put(phoff + 56 + 40, total - dyn, 8);
    return elf;
}

bool bytes_equal(const std::vector<std::uint8_t>& b, std::size_t off, const char* text) {
    const std::size_t n = std::strlen(text) + 1;  // include NUL
    return off + n <= b.size() && std::memcmp(b.data() + off, text, n) == 0;
}

// SafVfs::rename_file over the SAF rename / move calls, checked through the backend tree.
void test_saf_vfs_rename() {
    using namespace wse::platform;

    auto read_str = [](Vfs& vfs, std::string_view path) {
        std::string out;
        return vfs.read_text(path, out) ? out : std::string("<read failed>");
    };

    // Backend without rename / move support: SafTreeBackend defaults return false.
    class NoRenameSafBackend final : public SafTreeBackend {
    public:
        explicit NoRenameSafBackend(std::shared_ptr<MemorySafBackend> inner) : inner_(std::move(inner)) {}
        [[nodiscard]] std::string root_document_id() const override { return inner_->root_document_id(); }
        [[nodiscard]] bool list_children(std::string_view parent,
                                         std::vector<SafChildEntry>& out) override {
            return inner_->list_children(parent, out);
        }
        [[nodiscard]] bool read_document(std::string_view doc, std::vector<std::uint8_t>& out) override {
            return inner_->read_document(doc, out);
        }
        [[nodiscard]] bool write_document(std::string_view parent, std::string_view existing,
                                          std::string_view name, std::span<const std::uint8_t> data,
                                          std::string& out) override {
            return inner_->write_document(parent, existing, name, data, out);
        }
        [[nodiscard]] bool create_directory(std::string_view parent, std::string_view name,
                                            std::string& out) override {
            return inner_->create_directory(parent, name, out);
        }
        [[nodiscard]] bool delete_document(std::string_view doc) override {
            return inner_->delete_document(doc);
        }

    private:
        std::shared_ptr<MemorySafBackend> inner_;
    };

    // 1. Same-directory rename.
    auto backend = std::make_shared<MemorySafBackend>();
    SafVfs saf(backend);
    expect(saf.make_dir("a"), "saf rename: make a");
    expect(saf.write_text("a/x.tmp", "PAYLOAD"), "saf rename: write a/x.tmp");
    backend->reset_call_counts();
    expect(saf.rename_file("a/x.tmp", "a/x.bin"), "saf rename: same-dir rename succeeds");
    expect(backend->rename_document_calls == 1 && backend->move_document_calls == 0,
           "saf rename: same-dir uses rename_document only");
    expect(read_str(saf, "a/x.bin") == "PAYLOAD", "saf rename: bytes follow the renamed file");
    expect(!saf.is_file("a/x.tmp"), "saf rename: source name gone");
    std::vector<VfsEntry> listing;
    expect(saf.list_dir("a", listing) && listing.size() == 1 && listing[0].name == "x.bin",
           "saf rename: list_dir shows x.bin only");

    // 2. Cross-directory move plus rename; verified by a fresh index over the backend tree.
    expect(saf.make_dir("b"), "saf rename: make b");
    backend->reset_call_counts();
    expect(saf.rename_file("a/x.bin", "b/y.bin"), "saf rename: cross-dir move+rename succeeds");
    expect(backend->move_document_calls == 1 && backend->rename_document_calls == 1,
           "saf rename: cross-dir uses move_document then rename_document");
    expect(read_str(saf, "b/y.bin") == "PAYLOAD" && !saf.is_file("a/x.bin"),
           "saf rename: cross-dir source gone, target readable in live index");
    SafVfs fresh(backend);
    expect(fresh.index_complete() && fresh.is_file("b/y.bin") && !fresh.is_file("a/x.bin"),
           "saf rename: fresh rebuild_index sees b/y.bin in the backend tree");
    expect(read_str(fresh, "b/y.bin") == "PAYLOAD", "saf rename: backend bytes moved with the file");
    expect(fresh.list_dir("a", listing) && listing.empty(), "saf rename: source directory now empty");

    // 3. Replacing an existing target deletes it first; counts stay consistent.
    expect(saf.write_text("a/x.tmp", "NEW"), "saf rename: write replacement source");
    expect(saf.write_text("a/x.bin", "OLD"), "saf rename: write existing target");
    const std::size_t files_before = saf.index_file_count();
    expect(saf.rename_file("a/x.tmp", "a/x.bin"), "saf rename: replace existing target succeeds");
    expect(read_str(saf, "a/x.bin") == "NEW", "saf rename: target holds the source content");
    expect(!saf.is_file("a/x.tmp"), "saf rename: replaced source gone");
    expect(saf.index_file_count() == files_before - 1, "saf rename: replace drops one file count");
    SafVfs replaced(backend);
    expect(replaced.index_file_count() == saf.index_file_count() &&
               read_str(replaced, "a/x.bin") == "NEW",
           "saf rename: fresh index agrees with live counts after replace");

    // 4. Failure cases leave the tree unchanged.
    expect(!saf.rename_file("b/y.bin", "nope/y.bin"), "saf rename: missing target parent fails");
    expect(!saf.rename_file("b", "c"), "saf rename: directory source fails");
    expect(!saf.rename_file("a/missing.bin", "a/z.bin"), "saf rename: missing source fails");
    expect(read_str(saf, "b/y.bin") == "PAYLOAD" && saf.is_dir("b") && !saf.is_file("nope/y.bin"),
           "saf rename: failed renames leave the tree unchanged");

    // 5. Case-insensitive paths.
    expect(saf.make_dir("c"), "saf rename: make c");
    expect(saf.write_text("C/X.TMP", "CI"), "saf rename: write C/X.TMP");
    expect(saf.rename_file("c/x.tmp", "C/X.bin"), "saf rename: case-insensitive rename succeeds");
    expect(saf.is_file("c/x.BIN") && !saf.is_file("C/X.TMP") && read_str(saf, "c/x.bin") == "CI",
           "saf rename: case-insensitive lookups see the renamed file");

    // 6. Backend without rename / move support returns false and changes nothing.
    auto inner = std::make_shared<MemorySafBackend>();
    auto no_rename = std::make_shared<NoRenameSafBackend>(inner);
    SafVfs limited(no_rename);
    expect(limited.make_dir("a") && limited.make_dir("b"), "no-rename backend: seed dirs");
    expect(limited.write_text("a/x.tmp", "PAYLOAD"), "no-rename backend: seed file");
    expect(!limited.rename_file("a/x.tmp", "a/x.bin"),
           "no-rename backend: same-dir rename returns false");
    expect(!limited.rename_file("a/x.tmp", "b/x.bin"),
           "no-rename backend: cross-dir rename returns false");
    expect(read_str(limited, "a/x.tmp") == "PAYLOAD" && !limited.is_file("a/x.bin") &&
               !limited.is_file("b/x.bin"),
           "no-rename backend: live index unchanged");
    SafVfs limited_fresh(inner);
    expect(limited_fresh.is_file("a/x.tmp") && !limited_fresh.is_file("a/x.bin") &&
               !limited_fresh.is_file("b/x.bin") && limited_fresh.index_file_count() == 1,
           "no-rename backend: backend tree unchanged");
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

        // 2. ELF-aware `libhardware.so` -> `libpanvk_hw.so` patching and VFS staging.
        const TestElf elf = build_test_elf64();
        {
            std::vector<std::uint8_t> direct = elf.bytes;
            expect(patch_icd_libhardware_needed(direct) == 2U,
                   "DT_NEEDED and DT_VERNEED libhardware.so names rewritten");
            expect(bytes_equal(direct, elf.needed_name_off, "libpanvk_hw.so") &&
                       bytes_equal(direct, elf.verneed_name_off, "libpanvk_hw.so"),
                   "both dynamic-string references now name libpanvk_hw.so");
            expect(bytes_equal(direct, elf.decoy_off, "libhardware.so"),
                   "unreferenced .rodata copy of libhardware.so left untouched");
            expect(direct.size() == elf.bytes.size(), "patch preserves ELF size and offsets");
            expect(patch_icd_libhardware_needed(direct) == 0U, "patching is idempotent");

            std::vector<std::uint8_t> not_elf{'\0', 'l', 'i', 'b'};
            const std::string raw("\0libhardware.so\0", 16);
            not_elf.insert(not_elf.end(), raw.begin(), raw.end());
            expect(patch_icd_libhardware_needed(not_elf) == 0U && !is_little_endian_elf(not_elf),
                   "non-ELF bytes are never patched");
            std::vector<std::uint8_t> truncated(elf.bytes.begin(), elf.bytes.begin() + 120);
            expect(patch_icd_libhardware_needed(truncated) == 0U,
                   "truncated ELF (dynamic section out of range) is rejected safely");
        }

        MemoryVfs stage_vfs;
        std::string staged_path;
        std::size_t patched_refs = 0;
        expect(stage_driver_package_to_vfs(stage_vfs, "drivers/panvk", panvk_json, elf.bytes,
                                           meta, staged_path, &patched_refs, &err),
               "stage PanVK driver package into Vfs");
        expect(patched_refs == 2U, "staging rewrote both libhardware.so references");
        expect(staged_path == "drivers/panvk/libvulkan_panfrost.so", "staged ICD path matches");
        std::vector<std::uint8_t> staged_elf;
        expect(stage_vfs.read_file(staged_path, staged_elf) &&
                   staged_elf.size() == elf.bytes.size() &&
                   bytes_equal(staged_elf, elf.needed_name_off, "libpanvk_hw.so") &&
                   bytes_equal(staged_elf, elf.decoy_off, "libhardware.so"),
               "staged ICD patched in place preserving ELF offsets");

        const std::vector<std::uint8_t> text_payload{'h', 'e', 'l', 'l', 'o'};
        expect(!stage_driver_package_to_vfs(stage_vfs, "drivers/bad", panvk_json, text_payload,
                                            meta, staged_path, &patched_refs, &err) &&
                   !stage_vfs.is_file("drivers/bad/libvulkan_panfrost.so"),
               "staging rejects a .so payload that is not an ELF file");
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

        // update_buffer with a larger payload re-creates the mirrored VkBuffer but keeps the count.
        {
            VulkanRhiDevice vk_rhi(panvk_loader);
            const std::array<std::uint8_t, 16> small_data{1, 2, 3, 4};
            const BufferHandle ub = vk_rhi.create_buffer(BufferUsage::vertex, small_data);
            const std::array<std::uint8_t, 64> big_data{};
            std::string update_err;
            expect(ub != 0 && vk_rhi.update_buffer(ub, big_data, &update_err) &&
                       vk_rhi.live_vk_buffers() == 1U,
                   "VulkanRhiDevice update_buffer with a larger payload succeeds and keeps live VkBuffer count");
            vk_rhi.destroy_buffer(ub);
            expect(vk_rhi.live_vk_buffers() == 0U, "updated VkBuffer is destroyed cleanly");
        }

        expect(probe.instance_api_version == ((1U << 22U) | (3U << 12U)) && probe.texture_compression_bc &&
                   probe.max_image_dimension_2d == 4096U && !probe.used_hal_module,
               "probe reports instance API, BC feature, and 2D image limit");

        // Limits and features from the device are honoured.
        {
            auto small = std::make_shared<VulkanDriverLoader>();
            VulkanMockConfig cfg{};
            cfg.max_image_dimension_2d = 2U;
            cfg.texture_compression_bc = false;
            VulkanDriverProbeResult small_probe{};
            expect(small->load_mock_icd(cfg) && small->probe_and_create_device(small_probe) &&
                       !small_probe.texture_compression_bc,
                   "mock without BC support reports texture_compression_bc = false");
            VulkanRhiDevice rhi(small);
            const std::array<std::uint8_t, 64> px{};
            std::string tex_err;
            expect(rhi.create_texture_rgba8(4U, 4U, px, &tex_err) == 0 &&
                       tex_err.find("maxImageDimension2D") != std::string::npos &&
                       small->live_vk_images() == 0U,
                   "texture larger than maxImageDimension2D is rejected");
            expect(rhi.create_texture_rgba8(2U, 2U, std::span(px).first(16)) != 0,
                   "texture within maxImageDimension2D is accepted");
        }

        // Android hwvulkan HAL (`HMI`) entry path.
        {
            VulkanDriverLoader hal;
            std::string hal_err;
            VulkanDriverProbeResult hal_probe{};
            expect(hal.attach_hal_module(DriverMode::panvk, mock_vulkan_hal_module(), &hal_err) &&
                       hal.probe_and_create_device(hal_probe) && hal_probe.ok &&
                       hal_probe.used_hal_module && hal_probe.mode == DriverMode::panvk,
                   "hwvulkan HAL module opens HWVULKAN_DEVICE_0 and creates a device");
            const std::array<std::uintptr_t, 40> not_a_module{};
            expect(!hal.attach_hal_module(DriverMode::panvk, not_a_module.data(), &hal_err) &&
                       hal_err.find("tag") != std::string::npos && !hal.is_loaded(),
                   "hwvulkan attach rejects a symbol that is not a hw_module_t");
            expect(!hal.attach_hal_module(DriverMode::panvk, nullptr, &hal_err),
                   "hwvulkan attach rejects null");
        }

        // Move-assignment releases the overwritten loader's device (ASan/LSan
        // would report the leaked mock instance/device otherwise), and a
        // moved-from loader is safe to probe and reuse.
        {
            VulkanDriverLoader a;
            VulkanDriverLoader b;
            VulkanDriverProbeResult pa{};
            VulkanDriverProbeResult pb{};
            expect(a.load_mock_icd(VulkanMockConfig{}) && a.probe_and_create_device(pa) &&
                       b.load_mock_icd(VulkanMockConfig{}) && b.probe_and_create_device(pb),
                   "two mock loaders open devices");
            b = std::move(a);
            expect(b.has_device(), "move-assigned loader owns the source device");
            VulkanDriverProbeResult moved{};
            expect(!a.has_device() && !a.probe_and_create_device(moved) && !moved.error.empty(),
                   "moved-from loader reports an error instead of crashing");
            expect(a.load_mock_icd(VulkanMockConfig{}) && a.probe_and_create_device(moved),
                   "moved-from loader can be reloaded");
        }

        // Mock fallback is reported as such, never as a silent success.
        {
            std::uint64_t hash = 0;
            const VulkanDriverProbeResult fb =
                run_vulkan_mesh_smoke_test(DriverMode::turnip, "", "", true, hash);
            expect(fb.ok && fb.is_mock && fb.fell_back_to_mock && fb.error.empty() &&
                       fb.fallback_reason.find("no custom Vulkan ICD path") != std::string::npos,
                   "smoke test marks mock fallback and keeps the reason");
            std::string no_fb_err;
            const VulkanDriverProbeResult strict =
                run_vulkan_mesh_smoke_test(DriverMode::panvk, "", "", false, hash, &no_fb_err);
            expect(!strict.ok && !strict.error.empty() && no_fb_err == strict.error && hash == 0U,
                   "smoke test without fallback fails with the load error");
            const VulkanDriverProbeResult missing = run_vulkan_mesh_smoke_test(
                DriverMode::panvk, "/nonexistent/libvulkan_panfrost.so", "", false, hash);
            expect(!missing.ok && missing.error.find("failed to open Vulkan ICD") != std::string::npos,
                   "missing ICD file reports the dlopen failure");
        }

        // System Vulkan loader probe executes cleanly whether a host GPU is present or not.
        if (system_vulkan_tests_enabled()) {
            VulkanDriverLoader sys_loader;
            std::string sys_err;
            if (sys_loader.load_system(&sys_err)) {
                VulkanDriverProbeResult sys_probe{};
                (void)sys_loader.probe_and_create_device(sys_probe);
            } else {
                expect(!sys_err.empty(), "missing system Vulkan loader yields diagnostic");
            }
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
        // System mode needs the host loader (opt-in); without it, compare a
        // mock-backed custom-ICD run so three paths are still checked.
        std::uint64_t sys_smoke_hash = 0;
        std::uint64_t turnip_smoke_hash = 0;
        std::uint64_t panvk_smoke_hash = 0;
        const DriverMode third_mode =
            system_vulkan_tests_enabled() ? DriverMode::system : DriverMode::custom_icd;
        expect(run_vulkan_mesh_smoke_test(third_mode, "", "", true, sys_smoke_hash).ok &&
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

        // Near-plane clipping: a triangle with one vertex behind the camera is
        // clipped, not dropped (ground/terrain under the camera stays visible).
        {
            float persp[16]{};
            make_orbit_mvp_matrix(1.0f, 1.8f, 0.0f, 0.0f, persp);
            const std::array<MeshVertex, 3> crossing{{
                {-0.8f, -0.3f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
                {0.8f, -0.3f, 0.5f, 0, 0, 1, 1, 0, 0xFFFFFFFFU},
                {0.0f, -0.3f, 5.0f, 0, 0, 1, 0.5f, 1, 0xFFFFFFFFU},  // behind the eye (z=1.8)
            }};
            const BufferHandle cvb = sw_rhi.create_buffer(
                BufferUsage::vertex, as_u8_span(crossing.data(), sizeof(crossing)));
            const PipelineHandle no_cull = sw_rhi.create_pipeline(PipelineDesc{true, true, CullMode::none});
            MeshDrawCall cross_draw{};
            cross_draw.pipeline = no_cull;
            cross_draw.vertex_buffer = cvb;
            cross_draw.index_buffer = ib;
            cross_draw.index_count = 3U;
            std::memcpy(cross_draw.mvp, persp, sizeof(persp));
            std::vector<std::uint8_t> cross_frame;
            const std::array<MeshDrawCall, 1> cross_draws{cross_draw};
            expect(sw_rhi.render_pass_and_readback(pass, cross_draws, cross_frame, &err),
                   "near-plane crossing draw succeeds");
            std::size_t lit = 0;
            for (std::size_t i = 0; i < cross_frame.size(); i += 4U) {
                lit += (cross_frame[i] == 255U) ? 1U : 0U;
            }
            expect(lit > 0U, "triangle crossing the near plane is clipped and still drawn");

            // Degenerate / non-finite input never reaches the integer rasterizer.
            const std::array<MeshVertex, 3> wild{{
                {1.0e30f, 0.0f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
                {-1.0e30f, 1.0e30f, 0.5f, 0, 0, 1, 1, 0, 0xFFFFFFFFU},
                {0.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f, 0, 0, 1, 0, 1, 0xFFFFFFFFU},
            }};
            const BufferHandle wvb =
                sw_rhi.create_buffer(BufferUsage::vertex, as_u8_span(wild.data(), sizeof(wild)));
            MeshDrawCall wild_draw = cross_draw;
            wild_draw.vertex_buffer = wvb;
            const std::array<MeshDrawCall, 1> wild_draws{wild_draw};
            expect(sw_rhi.render_pass_and_readback(pass, wild_draws, cross_frame, &err),
                   "huge and NaN vertex coordinates are handled without UB");
            sw_rhi.destroy_buffer(wvb);
            sw_rhi.destroy_buffer(cvb);

            // Top-left fill rule: the two halves of a full-screen quad split along
            // a diagonal through pixel centres cover every pixel exactly once.
            const std::array<MeshVertex, 4> quad{{
                {-1.0f, -1.0f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
                {1.0f, -1.0f, 0.5f, 0, 0, 1, 1, 0, 0xFFFFFFFFU},
                {1.0f, 1.0f, 0.5f, 0, 0, 1, 1, 1, 0xFFFFFFFFU},
                {-1.0f, 1.0f, 0.5f, 0, 0, 1, 0, 1, 0xFFFFFFFFU},
            }};
            const std::array<std::uint16_t, 3> half_a{0, 1, 2};
            const std::array<std::uint16_t, 3> half_b{0, 2, 3};
            const BufferHandle qvb =
                sw_rhi.create_buffer(BufferUsage::vertex, as_u8_span(quad.data(), sizeof(quad)));
            const BufferHandle iba =
                sw_rhi.create_buffer(BufferUsage::index, as_u8_span(half_a.data(), sizeof(half_a)));
            const BufferHandle ibb =
                sw_rhi.create_buffer(BufferUsage::index, as_u8_span(half_b.data(), sizeof(half_b)));
            MeshDrawCall qa{};
            qa.pipeline = no_cull;
            qa.vertex_buffer = qvb;
            qa.index_buffer = iba;
            qa.index_count = 3U;
            MeshDrawCall qb = qa;
            qb.index_buffer = ibb;
            RenderPassDesc black = pass;
            black.clear_r = black.clear_g = black.clear_b = 0U;
            std::vector<std::uint8_t> fa;
            std::vector<std::uint8_t> fb;
            const std::array<MeshDrawCall, 1> da{qa};
            const std::array<MeshDrawCall, 1> db{qb};
            expect(sw_rhi.render_pass_and_readback(black, da, fa, &err) &&
                       sw_rhi.render_pass_and_readback(black, db, fb, &err),
                   "quad halves render");
            std::size_t both = 0;
            std::size_t neither = 0;
            std::size_t count_a = 0;
            for (std::size_t i = 0; i < fa.size(); i += 4U) {
                const bool in_a = fa[i] == 255U;
                const bool in_b = fb[i] == 255U;
                count_a += in_a ? 1U : 0U;
                both += (in_a && in_b) ? 1U : 0U;
                neither += (!in_a && !in_b) ? 1U : 0U;
            }
            expect(both == 0U && neither == 0U && count_a > 0U && count_a < 64U * 64U,
                   "shared diagonal edge: no pixel drawn twice, none missed");
            sw_rhi.destroy_buffer(ibb);
            sw_rhi.destroy_buffer(iba);
            sw_rhi.destroy_buffer(qvb);
            sw_rhi.destroy_pipeline(no_cull);
        }

        // BRF corner de-duplication.
        {
            wse::asset::BrfMesh dup = mesh;
            dup.face_corners.push_back(dup.face_corners[0]);  // corner 4 == corner 0
            dup.face_corners.push_back(dup.face_corners[2]);  // corner 5 == corner 2
            dup.faces[1] = wse::asset::BrfMeshFace{{4, 5, 3}};
            dup.faces.push_back(wse::asset::BrfMeshFace{{0, 1, 99}});  // corrupt: skipped
            std::vector<MeshVertex> dv;
            std::vector<std::uint16_t> di;
            build_rhi_mesh_from_brf(dup, dv, di);
            expect(dv.size() == 4U && di.size() == 6U && di[3] == di[0] && di[4] == di[2],
                   "identical BRF corners share one vertex; corrupt face dropped");
            std::vector<std::uint8_t> dup_frame;
            expect(render_textured_brf_mesh_frame(sw_rhi, dup, dds, 64U, 64U, mvp, dup_frame, &err) &&
                       hash_rgba8_frame(dup_frame) == sw_hash,
                   "de-duplicated mesh renders the identical frame");
        }

        sw_rhi.destroy_buffer(ib);
        sw_rhi.destroy_buffer(near_vb);
        sw_rhi.destroy_buffer(far_vb);
        sw_rhi.destroy_pipeline(pip);
        sw_rhi.destroy_texture(white_th);
    }
}

// update_buffer replaces live buffer contents, and MeshDrawCall::first_index lets one index
// buffer back several draws (checked against separate index buffers, and rejected when the
// range runs past the end of the buffer).
void test_rhi_update_buffer_and_first_index() {
    using namespace wse::platform;

    const auto as_u8_span = [](const void* ptr, std::size_t bytes) noexcept {
        return std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(ptr), bytes);
    };

    SoftwareRhiDevice sw_rhi;
    std::string err;
    const std::array<MeshVertex, 4> quad{{
        {-1.0f, -1.0f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
        {1.0f, -1.0f, 0.5f, 0, 0, 1, 1, 0, 0xFFFFFFFFU},
        {1.0f, 1.0f, 0.5f, 0, 0, 1, 1, 1, 0xFFFFFFFFU},
        {-1.0f, 1.0f, 0.5f, 0, 0, 1, 0, 1, 0xFFFFFFFFU},
    }};
    const std::array<std::uint16_t, 3> half_a{0, 1, 2};
    const std::array<std::uint16_t, 3> half_b{0, 2, 3};
    const std::array<std::uint16_t, 6> both{0, 1, 2, 0, 2, 3};
    const PipelineHandle no_cull = sw_rhi.create_pipeline(PipelineDesc{true, true, CullMode::none});
    const BufferHandle qvb =
        sw_rhi.create_buffer(BufferUsage::vertex, as_u8_span(quad.data(), sizeof(quad)));
    const BufferHandle iba =
        sw_rhi.create_buffer(BufferUsage::index, as_u8_span(half_a.data(), sizeof(half_a)));
    const BufferHandle ibb =
        sw_rhi.create_buffer(BufferUsage::index, as_u8_span(half_b.data(), sizeof(half_b)));
    const BufferHandle ibc =
        sw_rhi.create_buffer(BufferUsage::index, as_u8_span(both.data(), sizeof(both)));
    expect(qvb != 0U && iba != 0U && ibb != 0U && ibc != 0U && no_cull != 0U,
           "quad vertex, index, and pipeline resources are created");

    RenderPassDesc pass{};
    pass.width = 64U;
    pass.height = 64U;
    RenderPassDesc black = pass;
    black.clear_r = black.clear_g = black.clear_b = 0U;

    // 1. update_buffer rejects handle 0, destroyed handles, and empty payloads.
    const std::array<std::uint8_t, 4> payload{1, 2, 3, 4};
    expect(!sw_rhi.update_buffer(0U, payload, &err) && !err.empty(),
           "update_buffer rejects handle 0 and reports an error");
    err.clear();
    const BufferHandle gone =
        sw_rhi.create_buffer(BufferUsage::index, as_u8_span(half_a.data(), sizeof(half_a)));
    sw_rhi.destroy_buffer(gone);
    expect(!sw_rhi.update_buffer(gone, as_u8_span(half_a.data(), sizeof(half_a)), &err) &&
               !err.empty(),
           "update_buffer rejects a destroyed handle and reports an error");
    expect(!sw_rhi.update_buffer(qvb, std::span<const std::uint8_t>{}, &err),
           "update_buffer rejects empty data");

    // 2. One index buffer with two first_index draws matches two separate index buffers.
    MeshDrawCall qa{};
    qa.pipeline = no_cull;
    qa.vertex_buffer = qvb;
    qa.index_buffer = iba;
    qa.index_count = 3U;
    MeshDrawCall qb = qa;
    qb.index_buffer = ibb;
    MeshDrawCall split_a = qa;
    split_a.index_buffer = ibc;
    split_a.first_index = 0U;
    MeshDrawCall split_b = split_a;
    split_b.first_index = 3U;

    std::vector<std::uint8_t> separate;
    std::vector<std::uint8_t> combined;
    const std::array<MeshDrawCall, 2> separate_draws{qa, qb};
    const std::array<MeshDrawCall, 2> combined_draws{split_a, split_b};
    expect(sw_rhi.render_pass_and_readback(black, separate_draws, separate, &err) &&
               sw_rhi.render_pass_and_readback(black, combined_draws, combined, &err),
           "separate and first_index quad draws render");
    std::size_t lit = 0;
    for (std::size_t i = 0; i < combined.size(); i += 4U) {
        lit += (combined[i] == 255U) ? 1U : 0U;
    }
    expect(combined == separate && lit > 0U,
           "first_index draws from one index buffer match separate index buffers pixel for pixel");

    // 3. A draw whose first_index + index_count exceeds the buffer is rejected.
    MeshDrawCall overrun = split_a;
    overrun.first_index = 3U;
    overrun.index_count = 6U;
    const std::array<MeshDrawCall, 1> overrun_draws{overrun};
    std::vector<std::uint8_t> overrun_frame;
    expect(!sw_rhi.render_pass_and_readback(black, overrun_draws, overrun_frame, &err),
           "draw with first_index + index_count past the index buffer end is rejected");

    // 4. Moved vertex positions through the same live handle change the output.
    const std::array<MeshDrawCall, 1> one_draw{qa};
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
    expect(sw_rhi.render_pass_and_readback(black, one_draw, before, &err),
           "full-screen quad renders before the vertex update");
    const std::array<MeshVertex, 4> shrunk{{
        {-0.5f, -0.5f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
        {0.5f, -0.5f, 0.5f, 0, 0, 1, 1, 0, 0xFFFFFFFFU},
        {0.5f, 0.5f, 0.5f, 0, 0, 1, 1, 1, 0xFFFFFFFFU},
        {-0.5f, 0.5f, 0.5f, 0, 0, 1, 0, 1, 0xFFFFFFFFU},
    }};
    expect(sw_rhi.update_buffer(qvb, as_u8_span(shrunk.data(), sizeof(shrunk)), &err),
           "update_buffer replaces vertex data on a live handle");
    expect(sw_rhi.render_pass_and_readback(black, one_draw, after, &err) && after != before,
           "moved vertices change the rendered output through the same vertex buffer handle");

    sw_rhi.destroy_buffer(ibc);
    sw_rhi.destroy_buffer(ibb);
    sw_rhi.destroy_buffer(iba);
    sw_rhi.destroy_buffer(qvb);
    sw_rhi.destroy_pipeline(no_cull);
}

void test_p5_shader_modules_d3d9_and_material_rhi() {
    using namespace wse::platform;
    using namespace wse::asset;

    // 1. PresentationPath selection: system Vulkan loader with Android WSI vs direct custom ICDs.
    {
        VulkanDriverLoader sys_wsi;
        VulkanMockConfig sys_cfg{};
        sys_cfg.mode = DriverMode::system;
        sys_cfg.direct_icd = false;
        sys_cfg.expose_android_surface_wsi = true;
        sys_cfg.device_name = "System Mali-G78 (Mock Loader)";
        VulkanDriverProbeResult sys_probe{};
        expect(sys_wsi.load_mock_icd(sys_cfg) && sys_wsi.probe_and_create_device(sys_probe),
               "system WSI mock loader probes cleanly");
        expect(sys_probe.supports_android_surface_wsi &&
                   sys_probe.presentation_path == PresentationPath::android_surface_wsi &&
                   std::string_view(presentation_path_name(sys_probe.presentation_path)) ==
                       "android_surface_wsi",
               "system Vulkan loader with Android WSI selects android_surface_wsi");

        VulkanDriverLoader direct_wsi;
        VulkanMockConfig direct_wsi_cfg = sys_cfg;
        direct_wsi_cfg.mode = DriverMode::panvk;
        direct_wsi_cfg.direct_icd = true;
        direct_wsi_cfg.expose_android_surface_wsi = true;
        VulkanDriverProbeResult direct_wsi_probe{};
        expect(direct_wsi.load_mock_icd(direct_wsi_cfg) &&
                   direct_wsi.probe_and_create_device(direct_wsi_probe),
               "direct ICD with WSI flag probes cleanly");
        expect(direct_wsi_probe.used_direct_icd && direct_wsi_probe.supports_android_surface_wsi &&
                   direct_wsi_probe.presentation_path ==
                       PresentationPath::offscreen_hardware_buffer_bridge,
               "direct custom ICD selects offscreen_hardware_buffer_bridge even if WSI flag set");

        VulkanDriverLoader turnip_loader;
        VulkanMockConfig turnip_cfg{};
        turnip_cfg.mode = DriverMode::turnip;
        turnip_cfg.device_name = "Turnip Adreno 740 (Mock ICD)";
        turnip_cfg.vendor_id = 0x5143U;
        turnip_cfg.device_id = 0x0740U;
        turnip_cfg.direct_icd = true;
        turnip_cfg.expose_android_surface_wsi = false;
        VulkanDriverProbeResult turnip_probe{};
        expect(turnip_loader.load_mock_icd(turnip_cfg) &&
                   turnip_loader.probe_and_create_device(turnip_probe),
               "direct Turnip mock ICD probes cleanly");
        expect(turnip_probe.used_direct_icd && !turnip_probe.supports_android_surface_wsi &&
                   turnip_probe.presentation_path ==
                       PresentationPath::offscreen_hardware_buffer_bridge &&
                   std::string_view(presentation_path_name(turnip_probe.presentation_path)) ==
                       "offscreen_hardware_buffer_bridge",
               "direct custom Turnip ICD uses offscreen_hardware_buffer_bridge");

        VulkanDriverLoader panvk_loader;
        VulkanMockConfig panvk_cfg{};
        panvk_cfg.mode = DriverMode::panvk;
        panvk_cfg.direct_icd = true;
        panvk_cfg.expose_android_surface_wsi = false;
        VulkanDriverProbeResult panvk_probe{};
        expect(panvk_loader.load_mock_icd(panvk_cfg) &&
                   panvk_loader.probe_and_create_device(panvk_probe),
               "direct PanVK mock ICD probes cleanly");
        expect(panvk_probe.used_direct_icd &&
                   panvk_probe.presentation_path ==
                       PresentationPath::offscreen_hardware_buffer_bridge,
               "direct custom PanVK ICD uses offscreen_hardware_buffer_bridge");
    }

    // 2. D3D9 projection, half-pixel offset, and sRGB math helpers.
    {
        auto mul_pt = [](const float m[16], float x, float y, float z, float w,
                         float& ox, float& oy, float& oz, float& ow) noexcept {
            ox = m[0] * x + m[4] * y + m[8] * z + m[12] * w;
            oy = m[1] * x + m[5] * y + m[9] * z + m[13] * w;
            oz = m[2] * x + m[6] * y + m[10] * z + m[14] * w;
            ow = m[3] * x + m[7] * y + m[11] * z + m[15] * w;
        };

        float persp[16]{};
        make_d3d9_perspective_off_center_rh(-1.0f, 1.0f, -1.0f, 1.0f, 1.0f, 10.0f, persp);
        float cx = 0, cy = 0, cz = 0, cw = 0;
        mul_pt(persp, 0.0f, 0.0f, -1.0f, 1.0f, cx, cy, cz, cw);
        expect(cw > 0.0f && std::fabs(cz / cw) < 1.0e-5f,
               "D3D9 RH perspective maps z=-z_near to NDC z=0");
        mul_pt(persp, 0.0f, 0.0f, -10.0f, 1.0f, cx, cy, cz, cw);
        expect(cw > 0.0f && std::fabs((cz / cw) - 1.0f) < 1.0e-5f,
               "D3D9 RH perspective maps z=-z_far to NDC z=1");

        float ortho[16]{};
        make_d3d9_ortho_off_center_rh(0.0f, 800.0f, 600.0f, 0.0f, 0.0f, 1.0f, ortho);
        mul_pt(ortho, 0.0f, 600.0f, 0.0f, 1.0f, cx, cy, cz, cw);
        expect(std::fabs(cx + 1.0f) < 1.0e-5f && std::fabs(cy + 1.0f) < 1.0e-5f &&
                   std::fabs(cz) < 1.0e-5f,
               "D3D9 RH ortho maps bottom-left near to (-1,-1,0)");
        mul_pt(ortho, 800.0f, 0.0f, -1.0f, 1.0f, cx, cy, cz, cw);
        expect(std::fabs(cx - 1.0f) < 1.0e-5f && std::fabs(cy - 1.0f) < 1.0e-5f &&
                   std::fabs(cz - 1.0f) < 1.0e-5f,
               "D3D9 RH ortho maps top-right far to (+1,+1,1)");

        float px = 0.0f, py = 0.0f;
        apply_d3d9_half_pixel_offset_px(10.0f, 20.0f, px, py);
        expect(px == 10.5f && py == 20.5f, "half-pixel px helper adds (+0.5, +0.5)");
        apply_d3d9_half_pixel_offset_clip(0.0f, 0.0f, 1.0f, 100U, 50U, cx, cy);
        expect(std::fabs(cx - 0.01f) < 1.0e-6f && std::fabs(cy - 0.02f) < 1.0e-6f,
               "half-pixel clip helper shifts by (1/W, 1/H) at w = 1");
        apply_d3d9_half_pixel_offset_clip(0.0f, 0.0f, 2.0f, 100U, 50U, cx, cy, true);
        expect(std::fabs(cx - 0.02f) < 1.0e-6f && std::fabs(cy + 0.04f) < 1.0e-6f,
               "half-pixel clip helper scales by w and flips for D3D9 +Y-up NDC");
        expect(unorm_to_u8(std::numeric_limits<float>::quiet_NaN()) == 0U &&
                   linear_to_srgb_u8(std::numeric_limits<float>::quiet_NaN()) == 0U &&
                   srgb_u8_to_linear_f32(128U) == srgb_to_linear_f32(128.0f / 255.0f),
               "NaN-safe UNORM conversion and exact sRGB decode table");

        const float lin_128 = srgb_to_linear_f32(128.0f / 255.0f);
        expect(lin_128 > 0.214f && lin_128 < 0.218f, "srgb_to_linear_f32(128/255) ~= 0.2158");
        expect(linear_to_srgb_u8(srgb_to_linear_f32(180.0f / 255.0f)) == 180U,
               "linear_to_srgb_u8 round-trips encoded sRGB byte");
    }

    // 3. GLSL -> SPIR-V -> VulkanRhiDevice ShaderModule lifecycle + material RHI rendering.
    {
        MemoryVfs shader_vfs;
        shader_vfs.write_text(
            "GLShaders/techniques.xml",
            "<techniques>\n"
            "  <technique name=\"standart_skin_bump_nospec_high\">\n"
            "    <vertex_shader>skin_bump.vs</vertex_shader>\n"
            "    <fragment_shader>skin_bump.fs</fragment_shader>\n"
            "  </technique>\n"
            "</techniques>\n");
        shader_vfs.write_text("GLShadersOptimized/vs_standart_skin_bump_nospec_high.glsl",
                              "uniform mat4 matWorldViewProj;\n"
                              "attribute vec4 inPosition;\n"
                              "attribute vec4 inBlendWeights;\n"
                              "void main() { gl_Position = matWorldViewProj * inPosition; }\n");
        shader_vfs.write_text("GLShadersOptimized/fs_standart_skin_bump_nospec_high.glsl",
                              "uniform sampler2D diffuse_texture;\n"
                              "uniform sampler2D normal_texture;\n"
                              "void main() { gl_FragColor = texture2D(diffuse_texture, vec2(0.5)) + "
                              "texture2D(normal_texture, vec2(0.5)); }\n");

        ShaderTechniqueCatalog catalog{};
        std::string err;
        expect(load_warband_shader_technique_catalog(shader_vfs, "", catalog, &err),
               "load synthetic GLShaders technique catalog");
        const GlTechniqueEntry* tech = catalog.find("standart_skin_bump_nospec_high");
        expect(tech != nullptr, "find standart_skin_bump_nospec_high technique");
        if (tech == nullptr) {
            return;
        }

        const auto shader_compiler = create_glslang_shader_compiler();
        SpirvCompileOutput vs_out{};
        SpirvCompileOutput fs_out{};
        const bool compiled_ok =
            compile_warband_glsl_to_spirv(*shader_compiler, tech->vs_glsl, SpirvStage::Vertex,
                                          tech->name, VertexDeclType::BumpedSkinned, vs_out,
                                          tech->fs_glsl) &&
            compile_warband_glsl_to_spirv(*shader_compiler, tech->fs_glsl, SpirvStage::Fragment,
                                          tech->name, VertexDeclType::BumpedSkinned, fs_out,
                                          tech->vs_glsl);
        bool refl_ok = !vs_out.reflection.uniform_blocks.empty() &&
                       vs_out.reflection.uniform_blocks[0].binding == 0U &&
                       vs_out.reflection.uniform_blocks[0].size_bytes >= 64U &&
                       fs_out.reflection.samplers.size() == 2U;
        const auto& vs_words = vs_out.spirv_words;
        const auto& fs_words = fs_out.spirv_words;
        expect(compiled_ok && refl_ok && is_valid_spirv_module(vs_words) &&
                   is_valid_spirv_module(fs_words) && spirv_has_entry_point(vs_words, 0U, "main") &&
                   spirv_has_entry_point(fs_words, 4U, "main"),
               "compiled VS and FS SPIR-V 1.0 modules pass reflection, entry point, and header validation");

        auto vk_loader = std::make_shared<VulkanDriverLoader>();
        VulkanMockConfig cfg{};
        cfg.mode = DriverMode::panvk;
        cfg.direct_icd = true;
        VulkanDriverProbeResult probe{};
        expect(vk_loader->load_mock_icd(cfg) && vk_loader->probe_and_create_device(probe),
               "open PanVK mock device for shader module test");
        VulkanRhiDevice vk_rhi(vk_loader);
        SoftwareRhiDevice sw_rhi;

        ShaderModuleDesc vs_desc{};
        vs_desc.stage = RhiShaderStage::vertex;
        vs_desc.name = tech->name + "_vs";
        vs_desc.spirv_words = vs_words;
        ShaderModuleDesc fs_desc{};
        fs_desc.stage = RhiShaderStage::fragment;
        fs_desc.name = tech->name + "_fs";
        fs_desc.spirv_words = fs_words;

        const ShaderModuleHandle bad_vs =
            vk_rhi.create_shader_module(ShaderModuleDesc{RhiShaderStage::vertex, "main", "bad",
                                                         {0x12345678U, 1U}},
                                        &err);
        expect(bad_vs == 0U && !err.empty() && vk_rhi.live_vk_shader_modules() == 0U,
               "invalid SPIR-V words are rejected cleanly before vkCreateShaderModule");

        const ShaderModuleHandle vk_vs = vk_rhi.create_shader_module(vs_desc, &err);
        const ShaderModuleHandle vk_fs = vk_rhi.create_shader_module(fs_desc, &err);
        const ShaderModuleHandle sw_vs = sw_rhi.create_shader_module(vs_desc, &err);
        const ShaderModuleHandle sw_fs = sw_rhi.create_shader_module(fs_desc, &err);
        expect(vk_vs != 0U && vk_fs != 0U && vk_rhi.live_vk_shader_modules() == 2U,
               "VulkanRhiDevice creates and tracks VS + FS VkShaderModule objects");
        expect(sw_vs != 0U && sw_fs != 0U,
               "SoftwareRhiDevice creates VS + FS shader module handles");

        const auto as_u8_span = [](const void* ptr, std::size_t bytes) noexcept {
            return std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(ptr), bytes);
        };
        const std::array<MeshVertex, 4> quad{{
            {-0.8f, -0.8f, 0.4f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0xFFFFFFFFU},
            {0.8f, -0.8f, 0.4f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0xFFFFFFFFU},
            {0.8f, 0.8f, 0.4f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0xFFFFFFFFU},
            {-0.8f, 0.8f, 0.4f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0xFFFFFFFFU},
        }};
        const std::array<std::uint16_t, 6> quad_idx{0, 1, 2, 0, 2, 3};

        const BufferHandle sw_vb =
            sw_rhi.create_buffer(BufferUsage::vertex, as_u8_span(quad.data(), sizeof(quad)));
        const BufferHandle sw_ib =
            sw_rhi.create_buffer(BufferUsage::index, as_u8_span(quad_idx.data(), sizeof(quad_idx)));
        const BufferHandle vk_vb =
            vk_rhi.create_buffer(BufferUsage::vertex, as_u8_span(quad.data(), sizeof(quad)));
        const BufferHandle vk_ib =
            vk_rhi.create_buffer(BufferUsage::index, as_u8_span(quad_idx.data(), sizeof(quad_idx)));

        const std::array<std::uint8_t, 4> gray_px{160U, 160U, 160U, 255U};
        const std::array<std::uint8_t, 4> lit_nrm_px{210U, 230U, 210U, 255U};
        const std::array<std::uint8_t, 4> dark_nrm_px{25U, 25U, 160U, 255U};
        const std::array<std::uint8_t, 4> bright_spec_px{255U, 255U, 255U, 255U};
        const std::array<std::uint8_t, 4> zero_spec_px{0U, 0U, 0U, 255U};

        const TextureHandle sw_diff = sw_rhi.create_texture_rgba8(1U, 1U, gray_px);
        const TextureHandle sw_nrm_lit = sw_rhi.create_texture_rgba8(1U, 1U, lit_nrm_px);
        const TextureHandle sw_nrm_dark = sw_rhi.create_texture_rgba8(1U, 1U, dark_nrm_px);
        const TextureHandle sw_spec_hi = sw_rhi.create_texture_rgba8(1U, 1U, bright_spec_px);
        const TextureHandle sw_spec_lo = sw_rhi.create_texture_rgba8(1U, 1U, zero_spec_px);

        const TextureHandle vk_diff = vk_rhi.create_texture_rgba8(1U, 1U, gray_px);
        const TextureHandle vk_nrm_lit = vk_rhi.create_texture_rgba8(1U, 1U, lit_nrm_px);
        const TextureHandle vk_spec_hi = vk_rhi.create_texture_rgba8(1U, 1U, bright_spec_px);

        PipelineDesc lit_spec_desc{};
        lit_spec_desc.cull_mode = CullMode::none;
        lit_spec_desc.vertex_shader = sw_vs;
        lit_spec_desc.fragment_shader = sw_fs;
        lit_spec_desc.enable_lighting = true;
        lit_spec_desc.use_normal_map = true;
        lit_spec_desc.use_specular_map = true;
        lit_spec_desc.specular_coefficient = 0.65f;
        lit_spec_desc.specular_power = 24.0f;

        PipelineDesc vk_lit_desc = lit_spec_desc;
        vk_lit_desc.vertex_shader = vk_vs;
        vk_lit_desc.fragment_shader = vk_fs;

        const PipelineHandle sw_lit_pip = sw_rhi.create_pipeline(lit_spec_desc);
        const PipelineHandle vk_lit_pip = vk_rhi.create_pipeline(vk_lit_desc);

        RenderPassDesc pass{};
        pass.width = 64U;
        pass.height = 64U;
        pass.clear_r = 10U;
        pass.clear_g = 12U;
        pass.clear_b = 16U;

        MeshDrawCall sw_draw{};
        sw_draw.pipeline = sw_lit_pip;
        sw_draw.vertex_buffer = sw_vb;
        sw_draw.index_buffer = sw_ib;
        sw_draw.texture = sw_diff;
        sw_draw.normal_texture = sw_nrm_lit;
        sw_draw.specular_texture = sw_spec_hi;
        sw_draw.index_count = 6U;

        MeshDrawCall vk_draw = sw_draw;
        vk_draw.pipeline = vk_lit_pip;
        vk_draw.vertex_buffer = vk_vb;
        vk_draw.index_buffer = vk_ib;
        vk_draw.texture = vk_diff;
        vk_draw.normal_texture = vk_nrm_lit;
        vk_draw.specular_texture = vk_spec_hi;

        const std::size_t center = (32U * 64U + 32U) * 4U;
        std::vector<std::uint8_t> f_lit_hi;
        std::vector<std::uint8_t> f_vk_lit;
        std::vector<std::uint8_t> f_lit_lo_spec;
        std::vector<std::uint8_t> f_lit_dark_nrm;

        const std::array<MeshDrawCall, 1> d_sw_hi{sw_draw};
        const std::array<MeshDrawCall, 1> d_vk_hi{vk_draw};
        expect(sw_rhi.render_pass_and_readback(pass, d_sw_hi, f_lit_hi, &err) &&
                   vk_rhi.render_pass_and_readback(pass, d_vk_hi, f_vk_lit, &err),
               "lit normal+specular material renders on both SoftwareRhiDevice and VulkanRhiDevice");
        expect(f_lit_hi == f_vk_lit,
               "SoftwareRhiDevice and VulkanRhiDevice produce identical lit material output");

        sw_draw.specular_texture = sw_spec_lo;
        const std::array<MeshDrawCall, 1> d_sw_lo_spec{sw_draw};
        expect(sw_rhi.render_pass_and_readback(pass, d_sw_lo_spec, f_lit_lo_spec, &err),
               "lit material without specular reflection renders");
        expect(f_lit_hi[center] > f_lit_lo_spec[center],
               "specular highlight boosts center brightness when specular mask is 255");

        sw_draw.normal_texture = sw_nrm_dark;
        const std::array<MeshDrawCall, 1> d_sw_dark{sw_draw};
        expect(sw_rhi.render_pass_and_readback(pass, d_sw_dark, f_lit_dark_nrm, &err),
               "lit material with away-facing normal map renders");
        expect(f_lit_lo_spec[center] > f_lit_dark_nrm[center],
               "tangent-space normal map modulates Blinn-Phong diffuse irradiance");

        // Alpha test (`GreaterEqual136`) must discard BEFORE writing depth.
        const std::array<std::uint8_t, 16> alpha_cutout_2x2{
            0U,   255U, 0U, 64U,   0U, 255U, 0U, 220U,
            0U,   255U, 0U, 64U,   0U, 255U, 0U, 220U,
        };
        const std::array<std::uint8_t, 4> far_red_px{220U, 20U, 20U, 255U};
        const TextureHandle sw_cutout_tex = sw_rhi.create_texture_rgba8(2U, 2U, alpha_cutout_2x2);
        const TextureHandle sw_red_tex = sw_rhi.create_texture_rgba8(1U, 1U, far_red_px);

        const std::array<MeshVertex, 4> far_quad{{
            {-0.8f, -0.8f, 0.8f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
            {0.8f, -0.8f, 0.8f, 0, 0, 1, 1, 0, 0xFFFFFFFFU},
            {0.8f, 0.8f, 0.8f, 0, 0, 1, 1, 1, 0xFFFFFFFFU},
            {-0.8f, 0.8f, 0.8f, 0, 0, 1, 0, 1, 0xFFFFFFFFU},
        }};
        const BufferHandle far_vb =
            sw_rhi.create_buffer(BufferUsage::vertex, as_u8_span(far_quad.data(), sizeof(far_quad)));

        PipelineDesc cutout_pip_desc{};
        cutout_pip_desc.depth_test = true;
        cutout_pip_desc.depth_write = true;
        cutout_pip_desc.cull_mode = CullMode::none;
        cutout_pip_desc.alpha_test = true;
        cutout_pip_desc.alpha_ref = 136U;
        const PipelineHandle sw_cutout_pip = sw_rhi.create_pipeline(cutout_pip_desc);

        PipelineDesc unlit_opaque_desc{};
        unlit_opaque_desc.depth_test = true;
        unlit_opaque_desc.depth_write = true;
        unlit_opaque_desc.cull_mode = CullMode::none;
        const PipelineHandle sw_unlit_pip = sw_rhi.create_pipeline(unlit_opaque_desc);

        MeshDrawCall near_cutout{};
        near_cutout.pipeline = sw_cutout_pip;
        near_cutout.vertex_buffer = sw_vb;
        near_cutout.index_buffer = sw_ib;
        near_cutout.texture = sw_cutout_tex;
        near_cutout.index_count = 6U;

        MeshDrawCall far_opaque{};
        far_opaque.pipeline = sw_unlit_pip;
        far_opaque.vertex_buffer = far_vb;
        far_opaque.index_buffer = sw_ib;
        far_opaque.texture = sw_red_tex;
        far_opaque.index_count = 6U;

        std::vector<std::uint8_t> f_cutout;
        const std::array<MeshDrawCall, 2> d_cutout{near_cutout, far_opaque};
        expect(sw_rhi.render_pass_and_readback(pass, d_cutout, f_cutout, &err),
               "alpha cutout pass renders");
        const std::size_t left_mid = (32U * 64U + 16U) * 4U;
        const std::size_t right_mid = (32U * 64U + 48U) * 4U;
        expect(f_cutout[left_mid + 0U] == 220U && f_cutout[left_mid + 1U] == 20U &&
                   f_cutout[right_mid + 0U] == 0U && f_cutout[right_mid + 1U] == 255U,
               "alpha-tested discard occurs before depth write, revealing far quad");

        // Alpha & additive blending.
        const std::array<std::uint8_t, 4> blend_src_px{0U, 200U, 100U, 128U};
        const TextureHandle sw_blend_tex = sw_rhi.create_texture_rgba8(1U, 1U, blend_src_px);

        PipelineDesc alpha_blend_desc{};
        alpha_blend_desc.cull_mode = CullMode::none;
        alpha_blend_desc.depth_write = false;
        alpha_blend_desc.blend_mode = BlendMode::alpha;
        const PipelineHandle sw_alpha_pip = sw_rhi.create_pipeline(alpha_blend_desc);

        PipelineDesc add_blend_desc = alpha_blend_desc;
        add_blend_desc.blend_mode = BlendMode::additive;
        const PipelineHandle sw_add_pip = sw_rhi.create_pipeline(add_blend_desc);

        RenderPassDesc blend_pass = pass;
        blend_pass.clear_r = 200U;
        blend_pass.clear_g = 40U;
        blend_pass.clear_b = 20U;

        MeshDrawCall blend_draw = far_opaque;
        blend_draw.pipeline = sw_alpha_pip;
        blend_draw.texture = sw_blend_tex;
        std::vector<std::uint8_t> f_alpha;
        const std::array<MeshDrawCall, 1> d_alpha{blend_draw};
        expect(sw_rhi.render_pass_and_readback(blend_pass, d_alpha, f_alpha, &err),
               "alpha blend draw renders");
        expect(f_alpha[center + 0U] >= 98U && f_alpha[center + 0U] <= 102U &&
                   f_alpha[center + 1U] >= 118U && f_alpha[center + 1U] <= 122U,
               "alpha blend interpolates src and dst accurately");

        blend_draw.pipeline = sw_add_pip;
        std::vector<std::uint8_t> f_add;
        const std::array<MeshDrawCall, 1> d_add{blend_draw};
        expect(sw_rhi.render_pass_and_readback(blend_pass, d_add, f_add, &err),
               "additive blend draw renders");
        expect(f_add[center + 0U] == 200U && f_add[center + 1U] == 240U &&
                   f_add[center + 2U] == 120U,
               "additive blend sums channels with saturation");

        // sRGB texture decode vs sRGB framebuffer encode round-trip.
        const std::array<std::uint8_t, 4> srgb_128_px{128U, 128U, 128U, 255U};
        const TextureHandle sw_srgb_tex = sw_rhi.create_texture_rgba8(1U, 1U, srgb_128_px);

        PipelineDesc srgb_in_only{};
        srgb_in_only.cull_mode = CullMode::none;
        srgb_in_only.srgb_texture = true;
        srgb_in_only.srgb_framebuffer = false;
        const PipelineHandle sw_srgb_in_pip = sw_rhi.create_pipeline(srgb_in_only);

        PipelineDesc srgb_roundtrip = srgb_in_only;
        srgb_roundtrip.srgb_framebuffer = true;
        const PipelineHandle sw_srgb_rt_pip = sw_rhi.create_pipeline(srgb_roundtrip);

        MeshDrawCall srgb_draw = far_opaque;
        srgb_draw.texture = sw_srgb_tex;
        srgb_draw.pipeline = sw_srgb_in_pip;
        std::vector<std::uint8_t> f_srgb_lin;
        std::vector<std::uint8_t> f_srgb_rt;
        const std::array<MeshDrawCall, 1> d_srgb_in{srgb_draw};
        srgb_draw.pipeline = sw_srgb_rt_pip;
        const std::array<MeshDrawCall, 1> d_srgb_rt{srgb_draw};
        expect(sw_rhi.render_pass_and_readback(pass, d_srgb_in, f_srgb_lin, &err) &&
                   sw_rhi.render_pass_and_readback(pass, d_srgb_rt, f_srgb_rt, &err),
               "sRGB decode and round-trip draws succeed");
        expect(f_srgb_lin[center] >= 54U && f_srgb_lin[center] <= 56U &&
                   f_srgb_rt[center] == 128U,
               "sRGB decode produces ~55 linear and sRGB encode restores 128");

        // D3D9 half-pixel compensation shifts left edge from x_px=15.3 to x_px=15.8.
        const float x_left_ndc = (15.3f / 32.0f) - 1.0f;
        const std::array<MeshVertex, 4> edge_quad{{
            {x_left_ndc, -0.5f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
            {0.5f, -0.5f, 0.5f, 0, 0, 1, 1, 0, 0xFFFFFFFFU},
            {0.5f, 0.5f, 0.5f, 0, 0, 1, 1, 1, 0xFFFFFFFFU},
            {x_left_ndc, 0.5f, 0.5f, 0, 0, 1, 0, 1, 0xFFFFFFFFU},
        }};
        const BufferHandle edge_vb =
            sw_rhi.create_buffer(BufferUsage::vertex, as_u8_span(edge_quad.data(), sizeof(edge_quad)));

        PipelineDesc hp_off_desc{};
        hp_off_desc.cull_mode = CullMode::none;
        hp_off_desc.compensate_d3d9_half_pixel = false;
        const PipelineHandle sw_hp_off = sw_rhi.create_pipeline(hp_off_desc);
        PipelineDesc hp_on_desc = hp_off_desc;
        hp_on_desc.compensate_d3d9_half_pixel = true;
        const PipelineHandle sw_hp_on = sw_rhi.create_pipeline(hp_on_desc);

        MeshDrawCall hp_draw = far_opaque;
        hp_draw.vertex_buffer = edge_vb;
        hp_draw.texture = sw_red_tex;
        hp_draw.pipeline = sw_hp_off;
        std::vector<std::uint8_t> f_hp_off;
        std::vector<std::uint8_t> f_hp_on;
        const std::array<MeshDrawCall, 1> d_hp_off{hp_draw};
        hp_draw.pipeline = sw_hp_on;
        const std::array<MeshDrawCall, 1> d_hp_on{hp_draw};
        expect(sw_rhi.render_pass_and_readback(pass, d_hp_off, f_hp_off, &err) &&
                   sw_rhi.render_pass_and_readback(pass, d_hp_on, f_hp_on, &err),
               "D3D9 half-pixel compensation draws succeed");
        const std::size_t col15 = (32U * 64U + 15U) * 4U;
        const std::size_t col16 = (32U * 64U + 16U) * 4U;
        expect(f_hp_off[col15] == 220U && f_hp_on[col15] == pass.clear_r &&
                   f_hp_on[col16] == 220U,
               "compensate_d3d9_half_pixel shifts x_px=15.3 across pixel center 15.5");

        // Cleanup all handles and verify zero leaked VkShaderModules.
        sw_rhi.destroy_pipeline(sw_hp_on);
        sw_rhi.destroy_pipeline(sw_hp_off);
        sw_rhi.destroy_buffer(edge_vb);
        sw_rhi.destroy_pipeline(sw_srgb_rt_pip);
        sw_rhi.destroy_pipeline(sw_srgb_in_pip);
        sw_rhi.destroy_texture(sw_srgb_tex);
        sw_rhi.destroy_pipeline(sw_add_pip);
        sw_rhi.destroy_pipeline(sw_alpha_pip);
        sw_rhi.destroy_texture(sw_blend_tex);
        sw_rhi.destroy_pipeline(sw_unlit_pip);
        sw_rhi.destroy_pipeline(sw_cutout_pip);
        sw_rhi.destroy_buffer(far_vb);
        sw_rhi.destroy_texture(sw_red_tex);
        sw_rhi.destroy_texture(sw_cutout_tex);
        sw_rhi.destroy_pipeline(sw_lit_pip);
        sw_rhi.destroy_pipeline(vk_lit_pip);
        sw_rhi.destroy_texture(sw_spec_lo);
        sw_rhi.destroy_texture(sw_spec_hi);
        sw_rhi.destroy_texture(sw_nrm_dark);
        sw_rhi.destroy_texture(sw_nrm_lit);
        sw_rhi.destroy_texture(sw_diff);
        sw_rhi.destroy_buffer(sw_ib);
        sw_rhi.destroy_buffer(sw_vb);
        sw_rhi.destroy_shader_module(sw_fs);
        sw_rhi.destroy_shader_module(sw_vs);

        vk_rhi.destroy_texture(vk_spec_hi);
        vk_rhi.destroy_texture(vk_nrm_lit);
        vk_rhi.destroy_texture(vk_diff);
        vk_rhi.destroy_buffer(vk_ib);
        vk_rhi.destroy_buffer(vk_vb);
        vk_rhi.destroy_shader_module(vk_fs);
        vk_rhi.destroy_shader_module(vk_vs);
        expect(vk_rhi.live_vk_shader_modules() == 0U && vk_rhi.live_vk_buffers() == 0U &&
                   vk_rhi.live_vk_images() == 0U,
               "VulkanRhiDevice cleans up all shader modules, buffers, and images");
    }
}

// Regression tests for the P5 review (each block names the defect).
void test_p5_rhi_review_fixes() {
    using namespace wse::platform;
    using namespace wse::asset;
    std::string err;

    // WSI is enabled (instance + VK_KHR_swapchain), not merely detected; a device
    // without VK_KHR_swapchain falls back to the offscreen bridge.
    {
        VulkanDriverLoader loader;
        VulkanMockConfig cfg{};
        cfg.mode = DriverMode::system;
        cfg.direct_icd = false;
        cfg.expose_android_surface_wsi = true;
        VulkanDriverProbeResult probe{};
        expect(loader.load_mock_icd(cfg) && loader.probe_and_create_device(probe) &&
                   probe.presentation_path == PresentationPath::android_surface_wsi &&
                   probe.wsi_instance_extensions_enabled && probe.swapchain_extension_enabled,
               "android_surface_wsi enables instance WSI and VK_KHR_swapchain");

        VulkanDriverLoader no_sc;
        cfg.expose_swapchain = false;
        VulkanDriverProbeResult probe2{};
        expect(no_sc.load_mock_icd(cfg) && no_sc.probe_and_create_device(probe2) &&
                   probe2.presentation_path == PresentationPath::offscreen_hardware_buffer_bridge &&
                   !probe2.swapchain_extension_enabled && !probe2.presentation_note.empty(),
               "missing VK_KHR_swapchain falls back to the offscreen bridge");

        VulkanDriverLoader direct;
        VulkanMockConfig dcfg{};
        VulkanDriverProbeResult probe3{};
        expect(direct.load_mock_icd(dcfg) && direct.probe_and_create_device(probe3) &&
                   !probe3.wsi_instance_extensions_enabled && !probe3.swapchain_extension_enabled,
               "direct ICD requests no WSI extensions");

        // Composed instance fallback: a 1.0-only ICD that also refuses WSI falls back
        // through INCOMPATIBLE_DRIVER -> apiVersion 1.0 -> EXTENSION_NOT_PRESENT -> offscreen bridge.
        VulkanDriverLoader composed;
        VulkanMockConfig ccfg{};
        ccfg.mode = DriverMode::system;
        ccfg.direct_icd = false;
        ccfg.expose_android_surface_wsi = true;
        ccfg.require_api_version_1_0 = true;
        ccfg.refuse_wsi_instance_extensions = true;
        VulkanDriverProbeResult probe4{};
        expect(composed.load_mock_icd(ccfg) && composed.probe_and_create_device(probe4) &&
                   probe4.presentation_path == PresentationPath::offscreen_hardware_buffer_bridge &&
                   !probe4.wsi_instance_extensions_enabled &&
                   probe4.instance_api_version == 0x00400000U,
               "composed 1.0 + WSI refusal instance fallbacks reach offscreen bridge");
    }

    const auto shader_compiler = wse::asset::create_glslang_shader_compiler();
    wse::asset::SpirvCompileOutput min_vs_out{};
    wse::asset::SpirvCompileOutput min_fs_out{};
    expect(wse::asset::compile_warband_glsl_to_spirv(*shader_compiler, "void main() {}",
                                                     wse::asset::SpirvStage::Vertex, "t",
                                                     wse::asset::VertexDeclType::Standard,
                                                     min_vs_out) &&
               wse::asset::compile_warband_glsl_to_spirv(*shader_compiler, "void main() {}",
                                                         wse::asset::SpirvStage::Fragment, "t",
                                                         wse::asset::VertexDeclType::Standard,
                                                         min_fs_out),
           "compile minimal VS and FS SPIR-V 1.0 modules");
    const auto& vs_words = min_vs_out.spirv_words;
    const auto& fs_words = min_fs_out.spirv_words;

    // Stage / entry-point validation.
    {
        SoftwareRhiDevice dev;
        expect(dev.create_shader_module(ShaderModuleDesc{RhiShaderStage::fragment, "main", "x", vs_words},
                                        &err) == 0U &&
                   dev.create_shader_module(ShaderModuleDesc{RhiShaderStage::vertex, "other", "x", vs_words},
                                            &err) == 0U &&
                   dev.create_shader_module(ShaderModuleDesc{RhiShaderStage::vertex, "", "x", vs_words},
                                            &err) == 0U,
               "shader module stage and entry point must match the SPIR-V OpEntryPoint");
        const ShaderModuleHandle vs = dev.create_shader_module(
            ShaderModuleDesc{RhiShaderStage::vertex, "main", "vs", vs_words}, &err);
        const ShaderModuleHandle fs = dev.create_shader_module(
            ShaderModuleDesc{RhiShaderStage::fragment, "main", "fs", fs_words}, &err);
        PipelineDesc swapped{};
        swapped.vertex_shader = fs;
        swapped.fragment_shader = vs;
        expect(vs != 0U && fs != 0U && dev.create_pipeline(swapped, &err) == 0U,
               "pipeline rejects a fragment module in the vertex slot");

        // Modules may be destroyed once the pipeline exists (VkShaderModule semantics).
        PipelineDesc pd{};
        pd.cull_mode = CullMode::none;
        pd.vertex_shader = vs;
        pd.fragment_shader = fs;
        const PipelineHandle pip = dev.create_pipeline(pd, &err);
        dev.destroy_shader_module(vs);
        dev.destroy_shader_module(fs);
        const std::array<MeshVertex, 3> tri{{
            {-0.5f, -0.5f, 0.5f, 0, 0, 1, 0, 0, 0xFF0000FFU},
            {0.5f, -0.5f, 0.5f, 0, 0, 1, 1, 0, 0xFF0000FFU},
            {0.0f, 0.5f, 0.5f, 0, 0, 1, 0, 1, 0xFF0000FFU},
        }};
        const std::array<std::uint16_t, 3> idx{0, 1, 2};
        const BufferHandle vb = dev.create_buffer(
            BufferUsage::vertex, std::span<const std::uint8_t>(
                                     reinterpret_cast<const std::uint8_t*>(tri.data()), sizeof(tri)));
        const BufferHandle ib = dev.create_buffer(
            BufferUsage::index, std::span<const std::uint8_t>(
                                    reinterpret_cast<const std::uint8_t*>(idx.data()), sizeof(idx)));
        MeshDrawCall draw{};
        draw.pipeline = pip;
        draw.vertex_buffer = vb;
        draw.index_buffer = ib;
        draw.index_count = 3U;
        RenderPassDesc pass{};
        std::vector<std::uint8_t> frame;
        const std::array<MeshDrawCall, 1> draws{draw};
        expect(pip != 0U && dev.render_pass_and_readback(pass, draws, frame, &err) &&
                   frame[(32U * 64U + 32U) * 4U] == 255U,
               "pipeline still draws after its shader modules are destroyed");

        // Lighting uses the model matrix: rotating the mesh 180 degrees about Y
        // turns a light-facing +Z normal away from the key light.
        PipelineDesc lit{};
        lit.cull_mode = CullMode::none;
        lit.enable_lighting = true;
        const PipelineHandle lit_pip = dev.create_pipeline(lit, &err);
        MeshDrawCall lit_draw = draw;
        lit_draw.pipeline = lit_pip;
        std::vector<std::uint8_t> f_front;
        std::vector<std::uint8_t> f_back;
        const std::array<MeshDrawCall, 1> d_front{lit_draw};
        lit_draw.model[0] = -1.0f;
        lit_draw.model[10] = -1.0f;
        const std::array<MeshDrawCall, 1> d_back{lit_draw};
        expect(dev.render_pass_and_readback(pass, d_front, f_front, &err) &&
                   dev.render_pass_and_readback(pass, d_back, f_back, &err) &&
                   f_front[(32U * 64U + 32U) * 4U] > f_back[(32U * 64U + 32U) * 4U],
               "normals are transformed by MeshDrawCall::model");

        // Alpha channel blends with the colour factors (SRCALPHA / INVSRCALPHA).
        PipelineDesc blend{};
        blend.cull_mode = CullMode::none;
        blend.depth_write = false;
        blend.blend_mode = BlendMode::alpha;
        const PipelineHandle blend_pip = dev.create_pipeline(blend, &err);
        MeshDrawCall bdraw = draw;
        bdraw.pipeline = blend_pip;
        const std::array<MeshVertex, 3> half_tri{{
            {-0.5f, -0.5f, 0.5f, 0, 0, 1, 0, 0, 0x80FFFFFFU},
            {0.5f, -0.5f, 0.5f, 0, 0, 1, 1, 0, 0x80FFFFFFU},
            {0.0f, 0.5f, 0.5f, 0, 0, 1, 0, 1, 0x80FFFFFFU},
        }};
        const BufferHandle hvb = dev.create_buffer(
            BufferUsage::vertex,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(half_tri.data()),
                                          sizeof(half_tri)));
        bdraw.vertex_buffer = hvb;
        RenderPassDesc bpass{};
        bpass.clear_a = 0U;
        std::vector<std::uint8_t> f_blend;
        const std::array<MeshDrawCall, 1> d_blend{bdraw};
        // a = 128/255: a*a + 0*(1-a) = 0.252 -> 64.
        expect(dev.render_pass_and_readback(bpass, d_blend, f_blend, &err) &&
                   f_blend[(32U * 64U + 32U) * 4U + 3U] >= 63U &&
                   f_blend[(32U * 64U + 32U) * 4U + 3U] <= 65U,
               "alpha blend applies SRCALPHA/INVSRCALPHA to the alpha channel");
        dev.destroy_buffer(hvb);
        dev.destroy_pipeline(blend_pip);
        dev.destroy_pipeline(lit_pip);
        dev.destroy_buffer(ib);
        dev.destroy_buffer(vb);
        dev.destroy_pipeline(pip);
    }

    // Material -> pipeline mapping.
    {
        MaterialRenderState st{};
        st.blend_mode = MaterialBlendMode::AdditiveAlpha;
        st.alpha_ref_u8 = 0U;
        st.depth_write = false;
        st.uses_specular = true;
        st.specular_coeff = 32.0f;
        st.specular_rgb = {0.2f, 0.5f, 0.1f};
        const PipelineDesc pd = make_pipeline_desc(st);
        expect(pd.blend_mode == BlendMode::additive_alpha && !pd.alpha_test && !pd.depth_write &&
                   pd.front_face == FrontFace::counter_clockwise && pd.specular_power == 32.0f &&
                   pd.specular_coefficient == 0.5f &&
                   to_rhi_shader_stage(SpirvStage::Fragment) == RhiShaderStage::fragment &&
                   to_spirv_stage(RhiShaderStage::vertex) == SpirvStage::Vertex,
               "make_pipeline_desc maps decoded material state onto PipelineDesc");

        // Retail BRF CCW triangle viewed under make_orbit_mvp_matrix must survive CullMode::back
        // with make_pipeline_desc (FrontFace::counter_clockwise) and be culled under clockwise.
        SoftwareRhiDevice wdev;
        const std::array<MeshVertex, 3> ccw_front{{
            {-0.5f, -0.5f, 0.0f, 0, 0, 1, 0, 0, 0xFF20E020U},
            {0.5f, -0.5f, 0.0f, 0, 0, 1, 1, 0, 0xFF20E020U},
            {0.0f, 0.5f, 0.0f, 0, 0, 1, 0.5f, 1, 0xFF20E020U},
        }};
        const std::array<std::uint16_t, 3> widx{0, 1, 2};
        const BufferHandle wvb = wdev.create_buffer(
            BufferUsage::vertex,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(ccw_front.data()),
                                          sizeof(ccw_front)));
        const BufferHandle wib = wdev.create_buffer(
            BufferUsage::index,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(widx.data()),
                                          sizeof(widx)));
        PipelineDesc pd_ccw = make_pipeline_desc(MaterialRenderState{});
        pd_ccw.depth_test = false;
        pd_ccw.depth_write = false;
        PipelineDesc pd_cw = pd_ccw;
        pd_cw.front_face = FrontFace::clockwise;
        const PipelineHandle p_ccw = wdev.create_pipeline(pd_ccw, &err);
        const PipelineHandle p_cw = wdev.create_pipeline(pd_cw, &err);
        MeshDrawCall wdraw{};
        wdraw.vertex_buffer = wvb;
        wdraw.index_buffer = wib;
        wdraw.index_count = 3U;
        make_orbit_mvp_matrix(1.0f, 1.8f, 0.0f, 0.0f, wdraw.mvp);
        RenderPassDesc wpass{};
        wpass.clear_r = 0;
        wpass.clear_g = 0;
        wpass.clear_b = 0;
        std::vector<std::uint8_t> f_ccw;
        std::vector<std::uint8_t> f_cw;
        wdraw.pipeline = p_ccw;
        const std::array<MeshDrawCall, 1> d_ccw{wdraw};
        expect(wdev.render_pass_and_readback(wpass, d_ccw, f_ccw, &err), "render CCW brf tri");
        wdraw.pipeline = p_cw;
        const std::array<MeshDrawCall, 1> d_cw{wdraw};
        expect(wdev.render_pass_and_readback(wpass, d_cw, f_cw, &err), "render CW culled tri");
        auto count_lit = [](const std::vector<std::uint8_t>& img) {
            std::size_t n = 0;
            for (std::size_t i = 0; i + 3 < img.size(); i += 4U) {
                if (img[i] != 0 || img[i + 1] != 0 || img[i + 2] != 0) ++n;
            }
            return n;
        };
        expect(count_lit(f_ccw) > 0U && count_lit(f_cw) == 0U,
               "make_pipeline_desc FrontFace::counter_clockwise keeps retail CCW triangle under orbit camera");
        wdev.destroy_pipeline(p_cw);
        wdev.destroy_pipeline(p_ccw);
        wdev.destroy_buffer(wib);
        wdev.destroy_buffer(wvb);
    }
}

// Regression tests for the second P4 review (each block names the defect).
void test_p4_review_fixes() {
    using namespace wse::platform;
    const MockVulkanObjectStats mock_before = mock_vulkan_object_stats();

    // Driver-mode ids are exact; display labels and free text are rejected.
    {
        DriverMode m = DriverMode::system;
        expect(parse_driver_mode("Turnip", m) && m == DriverMode::turnip &&
                   parse_driver_mode(" panvk ", m) && m == DriverMode::panvk &&
                   parse_driver_mode("custom_icd", m) && m == DriverMode::custom_icd &&
                   parse_driver_mode("SYSTEM", m) && m == DriverMode::system,
               "parse_driver_mode accepts stable ids case-insensitively");
        expect(!parse_driver_mode("not turnip", m) && !parse_driver_mode("PanVK (Mali direct ICD)", m) &&
                   !parse_driver_mode("System Vulkan", m) && !parse_driver_mode("", m),
               "parse_driver_mode rejects labels and substrings");
    }

    // libraryName allow-list.
    {
        expect(is_safe_driver_library_name("libvulkan_freedreno.so") &&
                   is_safe_driver_library_name("libvulkan-panfrost+v2.SO") &&
                   is_safe_driver_library_name("vulkan_icd.dll"),
               "ordinary driver library names are accepted");
        const std::string too_long = std::string(130, 'a') + ".so";
        expect(!is_safe_driver_library_name(".so") && !is_safe_driver_library_name(".hidden.so") &&
                   !is_safe_driver_library_name("-rf.so") && !is_safe_driver_library_name("a b.so") &&
                   !is_safe_driver_library_name("lib\x01.so") &&
                   !is_safe_driver_library_name("lib\xC3\xA9.so") &&
                   !is_safe_driver_library_name(too_long) && !is_safe_driver_library_name("lib..so") &&
                   !is_safe_driver_library_name("a/b.so") && !is_safe_driver_library_name("c:x.so"),
               "unsafe driver library names are rejected by the allow-list");
    }

    // meta.json: nested values are skipped, not fatal; strict default unchanged.
    {
        DriverPackageMeta meta{};
        std::string err;
        const std::string nested = R"({"schemaVersion":1,"libraryName":"libvulkan_freedreno.so",
            "name":"Turnip","supportedGpus":["a7xx",{"id":740}],"extra":{"k":[1,2,{}]},"minApi":28})";
        expect(parse_driver_meta_json(nested, meta, &err) && meta.min_api == 28 &&
                   meta.library_name == "libvulkan_freedreno.so",
               "meta.json with nested arrays/objects parses");
        expect(!parse_driver_meta_json(R"({"schemaVersion":1,"libraryName":"x.so","bad":[1,})", meta, &err),
               "malformed nested value is still rejected");
        std::map<std::string, std::string> strict;
        expect(!wse::core::wse_parse_json_object(R"({"a":[1]})", strict),
               "wse_parse_json_object stays strict by default (WSE dict ops)");

        DriverPackageMeta turnip{};
        turnip.inferred_mode = DriverMode::turnip;
        turnip.min_api = 30;
        expect(!check_driver_package_compat(turnip, 29, DriverMode::turnip, &err) &&
                   err.find("API 30") != std::string::npos,
               "package minApi above the device API level is refused");
        expect(!check_driver_package_compat(turnip, 34, DriverMode::panvk, &err) &&
                   check_driver_package_compat(turnip, 34, DriverMode::turnip, &err) &&
                   check_driver_package_compat(turnip, 0, DriverMode::system, &err),
               "package kind must match a specific requested mode");
    }

    // Staging replaces files atomically and writes meta.json last.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path root = fs::temp_directory_path(ec) / "wse_platform_test_stage";
        fs::remove_all(root, ec);
        HostVfs vfs(root);
        const std::string meta_json = R"({"schemaVersion":1,"libraryName":"libvulkan_panfrost.so","name":"PanVK"})";
        TestElf first = build_test_elf64();
        DriverPackageMeta meta{};
        std::string staged;
        std::string err;
        std::size_t patched = 0;
        expect(stage_driver_package_to_vfs(vfs, "drivers/panvk", meta_json, std::move(first.bytes), meta,
                                           staged, &patched, &err) &&
                   patched == 2U && vfs.is_file("drivers/panvk/meta.json"),
               "stage into a nested, missing directory");
        std::vector<std::uint8_t> old_bytes;
        expect(vfs.read_file(staged, old_bytes), "staged ICD readable");
#ifndef _WIN32
        // A reader that has the old library open (as dlopen keeps it mapped)
        // must keep seeing the old bytes after a re-import.
        std::FILE* held = std::fopen((root / "drivers/panvk/libvulkan_panfrost.so").c_str(), "rb");
#endif
        TestElf second = build_test_elf64();
        second.bytes.resize(second.bytes.size() + 64, 0xAB);  // different content and size
        expect(stage_driver_package_to_vfs(vfs, "drivers/panvk", meta_json, std::move(second.bytes), meta,
                                           staged, &patched, &err),
               "re-stage over an existing package");
#ifndef _WIN32
        if (held != nullptr) {
            std::vector<std::uint8_t> seen(old_bytes.size() + 128);
            const std::size_t got = std::fread(seen.data(), 1, seen.size(), held);
            std::fclose(held);
            seen.resize(got);
            expect(seen == old_bytes, "re-stage never rewrites the old ICD file in place");
        }
#endif
        std::vector<std::uint8_t> new_bytes;
        expect(vfs.read_file(staged, new_bytes) && new_bytes.size() == old_bytes.size() + 64 &&
                   !vfs.is_file("drivers/panvk/libvulkan_panfrost.so.partial") &&
                   !vfs.is_file("drivers/panvk/meta.json.partial"),
               "re-stage leaves the new ICD and no .partial files");
        expect(vfs.write_text("blocker", "x") &&
                   !stage_driver_package_to_vfs(vfs, "blocker/sub", meta_json,
                                                std::span<const std::uint8_t>(new_bytes), meta, staged,
                                                &patched, &err) &&
                   err.find("stage directory") != std::string::npos,
               "uncreatable stage directory is reported, not ignored");
        fs::remove_all(root, ec);

        MemoryVfs mem;
        expect(mem.write_text("a.txt", "1") && mem.make_dir("d") && mem.rename_file("a.txt", "d/B.txt") &&
                   !mem.is_file("a.txt") && mem.is_file("d/b.txt") && !mem.rename_file("d/b.txt", "missing/x") &&
                   !mem.rename_file("nope", "d/c.txt"),
               "MemoryVfs rename_file moves, renames, and checks parents");
    }

    // Software rasterizer: failures leave no frame; clear depth validated;
    // front-face winding is explicit; handles never alias after wrap.
    {
        SoftwareRhiDevice rhi;
        std::vector<std::uint8_t> frame(16, 7);
        RenderPassDesc pass{};
        pass.width = 8;
        pass.height = 8;
        MeshDrawCall bad{};
        bad.pipeline = 999;
        const std::array<MeshDrawCall, 1> bad_draws{bad};
        std::string err;
        expect(!rhi.render_pass_and_readback(pass, bad_draws, frame, &err) && frame.empty(),
               "failed render pass leaves an empty frame");
        pass.clear_depth = std::numeric_limits<float>::quiet_NaN();
        expect(!rhi.render_pass_and_readback(pass, {}, frame, &err) && frame.empty(),
               "NaN clear depth is rejected");
        pass.clear_depth = 1.0f;
        expect(rhi.render_pass_and_readback(pass, {}, frame, &err) && frame.size() == 8U * 8U * 4U &&
                   frame[4U * 63U] == pass.clear_r && frame[4U * 63U + 3U] == pass.clear_a,
               "empty pass clears every pixel");

        // Visually clockwise on screen (y down): (16,16) -> (48,16) -> (32,48).
        const std::array<MeshVertex, 3> cw{{
            {-0.5f, -0.5f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
            {0.5f, -0.5f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
            {0.0f, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0xFFFFFFFFU},
        }};
        const std::array<std::uint16_t, 3> idx{0, 1, 2};
        const auto u8 = [](const void* p, std::size_t n) {
            return std::span<const std::uint8_t>(static_cast<const std::uint8_t*>(p), n);
        };
        const BufferHandle vb = rhi.create_buffer(BufferUsage::vertex, u8(cw.data(), sizeof(cw)));
        const BufferHandle ib = rhi.create_buffer(BufferUsage::index, u8(idx.data(), sizeof(idx)));
        const auto drawn_pixels = [&](FrontFace face) {
            PipelineDesc desc{};
            desc.cull_mode = CullMode::back;
            desc.front_face = face;
            MeshDrawCall d{};
            d.pipeline = rhi.create_pipeline(desc);
            d.vertex_buffer = vb;
            d.index_buffer = ib;
            d.index_count = 3;
            RenderPassDesc p64{};
            std::vector<std::uint8_t> out;
            const std::array<MeshDrawCall, 1> ds{d};
            (void)rhi.render_pass_and_readback(p64, ds, out);
            rhi.destroy_pipeline(d.pipeline);
            int n = 0;
            for (std::size_t i = 0; i < out.size(); i += 4) n += (out[i] == 255U);
            return n;
        };
        expect(drawn_pixels(FrontFace::clockwise) > 0 && drawn_pixels(FrontFace::counter_clockwise) == 0,
               "back-face culling honours PipelineDesc::front_face (screen-clockwise triangle)");

        rhi.debug_set_next_handle(0xFFFFFFFFU);
        const PipelineHandle last = rhi.create_pipeline(PipelineDesc{});
        const PipelineHandle after = rhi.create_pipeline(PipelineDesc{});
        expect(last == 0xFFFFFFFFU && after != 0U && after != vb && after != ib,
               "handle allocation skips 0 and live handles after the counter wraps");
    }

    // Orbit camera: far plane follows distance; non-finite inputs stay finite.
    {
        float mvp[16]{};
        make_orbit_mvp_matrix(1.0f, 500.0f, 0.3f, 0.2f, mvp);
        // Clip-space z/w of the target (origin) is column 3.
        const float z_ndc = mvp[14] / mvp[15];
        expect(z_ndc > 0.0f && z_ndc < 1.0f, "target at distance 500 is inside the depth range");
        make_orbit_mvp_matrix(std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN(), 0.0f, mvp);
        bool finite = true;
        for (const float v : mvp) finite = finite && std::isfinite(v);
        expect(finite, "non-finite orbit inputs produce a finite matrix");
    }

    // Stale VkBuffer handle from an earlier device never destroys a live one.
    {
        auto loader = std::make_shared<VulkanDriverLoader>();
        VulkanDriverProbeResult pr{};
        expect(loader->load_mock_icd(VulkanMockConfig{}) && loader->probe_and_create_device(pr),
               "mock device for generation test");
        const std::uint64_t gen1 = loader->device_generation();
        const std::array<std::uint8_t, 4> data{1, 2, 3, 4};
        VulkanRhiDevice old_rhi(loader);
        const BufferHandle stale = old_rhi.create_buffer(BufferUsage::vertex, data);
        expect(loader->probe_and_create_device(pr) && loader->device_generation() != gen1 &&
                   gen1 != 0U,
               "re-probing opens a device with a new generation");
        VulkanRhiDevice new_rhi(loader);
        const BufferHandle fresh = new_rhi.create_buffer(BufferUsage::vertex, data);
        old_rhi.destroy_buffer(stale);
        expect(fresh != 0U && loader->live_vk_buffers() == 1U,
               "destroying a stale handle leaves the new device's buffer alive");
        new_rhi.destroy_buffer(fresh);
        expect(loader->live_vk_buffers() == 0U, "current-generation destroy still works");

        // Move-assignment gives no repeated generation.
        VulkanDriverLoader a;
        VulkanDriverLoader b;
        expect(a.load_mock_icd(VulkanMockConfig{}) && a.probe_and_create_device(pr) &&
                   b.load_mock_icd(VulkanMockConfig{}) && b.probe_and_create_device(pr) &&
                   a.device_generation() != b.device_generation(),
               "device generations are unique across loaders");
    }

    // BrfMeshRenderer matches the one-shot helper and re-renders without re-upload.
    {
        wse::asset::BrfMesh mesh{};
        mesh.name = "renderer_quad";
        mesh.positions = {{-0.6f, -0.6f, 0.0f}, {0.6f, -0.6f, 0.0f}, {0.6f, 0.6f, 0.0f}, {-0.6f, 0.6f, 0.0f}};
        for (std::size_t i = 0; i < 4U; ++i) {
            wse::asset::BrfFaceCorner c{};
            c.vertex_index = static_cast<std::int32_t>(i);
            c.color = 0xFFFFFFFFU;
            c.normal = {0.0f, 0.0f, 1.0f};
            c.uv = {static_cast<float>(i & 1U), static_cast<float>(i >> 1U)};
            mesh.face_corners.push_back(c);
        }
        mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 1, 2}});
        mesh.faces.push_back(wse::asset::BrfMeshFace{{0, 2, 3}});
        std::vector<std::byte> dds_bytes(128U + 64U, std::byte{0x80});
        const auto w32 = [&](std::size_t off, std::uint32_t v) {
            for (unsigned i = 0; i < 4U; ++i) dds_bytes[off + i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
        };
        dds_bytes[0] = std::byte{'D'}; dds_bytes[1] = std::byte{'D'}; dds_bytes[2] = std::byte{'S'}; dds_bytes[3] = std::byte{' '};
        for (std::size_t i = 4; i < 128; ++i) dds_bytes[i] = std::byte{0};
        w32(4, 124); w32(8, 0x100F); w32(12, 4); w32(16, 4); w32(20, 16);
        w32(76, 32); w32(80, 0x41); w32(88, 32);
        w32(92, 0x00FF0000U); w32(96, 0x0000FF00U); w32(100, 0x000000FFU); w32(104, 0xFF000000U);
        w32(108, 0x1000);
        wse::asset::DdsImageView dds{};
        std::string err;
        expect(wse::asset::parse_dds(dds_bytes, dds, &err), "parse 4x4 A8R8G8B8 DDS for renderer test");

        float mvp[16]{};
        make_orbit_mvp_matrix(1.0f, 1.8f, 0.0f, 0.0f, mvp);
        SoftwareRhiDevice rhi;
        std::vector<std::uint8_t> one_shot;
        expect(render_textured_brf_mesh_frame(rhi, mesh, dds, 32U, 32U, mvp, one_shot, &err, CullMode::none),
               "one-shot BRF render");
        std::vector<std::uint8_t> a;
        std::vector<std::uint8_t> b;
        {
            BrfMeshRenderer renderer;
            expect(renderer.upload(rhi, mesh, dds, &err, CullMode::none) &&
                       renderer.render(32U, 32U, mvp, a, &err) && renderer.render(32U, 32U, mvp, b, &err) &&
                       a == one_shot && b == one_shot,
                   "BrfMeshRenderer frames match the one-shot helper");
        }
        // The renderer released its objects: a fresh handle-free render fails cleanly.
        BrfMeshRenderer empty;
        expect(!empty.render(8U, 8U, mvp, a, &err) && a.empty(), "unuploaded BrfMeshRenderer refuses to render");
    }

    // Mock ICD accounting: every object created above was destroyed exactly once.
    {
        const MockVulkanObjectStats after = mock_vulkan_object_stats();
        expect(after.live_objects == mock_before.live_objects &&
                   after.leaked_at_device_destroy == mock_before.leaked_at_device_destroy &&
                   after.invalid_destroys == mock_before.invalid_destroys,
               "mock ICD: no leaked, early-orphaned, or double-destroyed Vulkan objects");
    }
}

// P10.4: launcher settings page keys in `config/performance.json` (quality preset, render scale,
// texture size cap, UI aspect, UI scale, language), the presets, the first-launch defaults, and
// the partial-JSON save the launcher's settings page uses.
void test_p10_launcher_settings() {
    using namespace wse::platform;

    {
        UiAspectMode m{};
        expect(parse_ui_aspect_mode("4:3", m) && m == UiAspectMode::Ratio4x3 &&
                   parse_ui_aspect_mode("16:9", m) && m == UiAspectMode::Ratio16x9 &&
                   parse_ui_aspect_mode("stretch", m) && m == UiAspectMode::Stretch &&
                   !parse_ui_aspect_mode("21:9", m),
               "P10.4 UI aspect keys parse");
        expect(ui_aspect_mode_key(UiAspectMode::Ratio16x9) == "16:9",
               "P10.4 UI aspect key round-trips");
        QualityPreset q{};
        expect(parse_quality_preset("low", q) && q == QualityPreset::Low &&
                   parse_quality_preset("medium", q) && q == QualityPreset::Medium &&
                   parse_quality_preset("high", q) && q == QualityPreset::High &&
                   parse_quality_preset("custom", q) && q == QualityPreset::Custom &&
                   !parse_quality_preset("ultra", q),
               "P10.4 quality preset keys parse");
        expect(quality_preset_key(QualityPreset::Medium) == "medium",
               "P10.4 quality preset key round-trips");
    }

    {
        const PerformanceSettings def{};
        expect(def.quality_preset == QualityPreset::Custom && def.render_scale == 1.0f &&
                   def.max_texture_size == 0U && def.ui_aspect == UiAspectMode::Ratio4x3 &&
                   def.ui_scale == 1.0f && def.language == "en",
               "P10.4 new settings defaults");

        PerformanceSettings s{};
        s.render_scale = 0.1f;
        s.ui_scale = 9.0f;
        s.max_texture_size = 3000U;
        s.language = "../etc";
        sanitize_performance_settings(s);
        expect(s.render_scale == 0.5f && s.ui_scale == 2.0f && s.max_texture_size == 2048U &&
                   s.language == "en",
               "P10.4 sanitize clamps scales, rounds the texture cap down to a power of two, "
               "and rejects a bad language");
        s.max_texture_size = 100U;
        sanitize_performance_settings(s);
        expect(s.max_texture_size == 256U, "P10.4 texture cap floor is 256");
        s.max_texture_size = 9000U;
        sanitize_performance_settings(s);
        expect(s.max_texture_size == 4096U, "P10.4 texture cap ceiling is 4096");
        s.language = "zh-CN";
        sanitize_performance_settings(s);
        expect(s.language == "zh-CN", "P10.4 a folder-style language name is kept");
        s.language = "abcdefghijklmnopq";  // 17 characters
        sanitize_performance_settings(s);
        expect(s.language == "en", "P10.4 a language longer than 16 characters is rejected");
    }

    {
        PerformanceSettings s{};
        s.quality_preset = QualityPreset::High;
        s.render_scale = 0.8f;
        s.max_texture_size = 1024U;
        s.ui_aspect = UiAspectMode::Stretch;
        s.ui_scale = 1.25f;
        s.language = "de";
        PerformanceSettings back{};
        std::string err;
        expect(deserialize_performance_settings_json(serialize_performance_settings_json(s),
                                                     back, &err) &&
                   back == s,
               "P10.4 new keys round-trip through JSON");
        PerformanceSettings old{};
        expect(deserialize_performance_settings_json(
                   "{\"version\":1,\"frame_rate_cap\":\"90\"}", old, &err) &&
                   old.frame_rate_cap == FrameRateCap::Fps90 &&
                   old.quality_preset == QualityPreset::Custom && old.language == "en",
               "P10.4 a file from before P10 loads with the new defaults");
        expect(!deserialize_performance_settings_json("{\"ui_aspect\":\"wide\"}", old, &err),
               "P10.4 an invalid ui_aspect is an error");
    }

    {
        PerformanceSettings s{};
        apply_quality_preset(s, QualityPreset::Low);
        expect(s.quality_preset == QualityPreset::Low && s.frame_rate_cap == FrameRateCap::Fps30 &&
                   s.render_scale == 0.75f && s.max_texture_size == 512U,
               "P10.4 low preset");
        apply_quality_preset(s, QualityPreset::Medium);
        expect(s.quality_preset == QualityPreset::Medium &&
                   s.frame_rate_cap == FrameRateCap::Fps30 && s.render_scale == 0.85f &&
                   s.max_texture_size == 1024U,
               "P10.4 medium preset");
        apply_quality_preset(s, QualityPreset::High);
        expect(s.quality_preset == QualityPreset::High &&
                   s.frame_rate_cap == FrameRateCap::Fps60 && s.render_scale == 1.0f &&
                   s.max_texture_size == 2048U,
               "P10.4 high preset");
        s.ui_scale = 1.5f;
        apply_quality_preset(s, QualityPreset::Custom);
        expect(s.quality_preset == QualityPreset::Custom &&
                   s.frame_rate_cap == FrameRateCap::Fps60 && s.max_texture_size == 2048U &&
                   s.ui_scale == 1.5f,
               "P10.4 custom keeps every value; presets leave UI settings alone");
        expect(kFirstLaunchQualityPreset == QualityPreset::Medium,
               "P10.4 first launch uses the medium preset");
    }

    {
        MemoryVfs vfs;
        PerformanceSettings s{};
        bool created = false;
        std::string err;
        expect(ensure_first_launch_settings(vfs, s, &created, &err) && created &&
                   s.quality_preset == QualityPreset::Medium &&
                   s.frame_rate_cap == FrameRateCap::Fps30 &&
                   vfs.is_file(performance_settings_vfs_path()),
               "P10.4 first launch writes the medium preset");
        PerformanceSettings again{};
        expect(ensure_first_launch_settings(vfs, again, &created, &err) && !created &&
                   again == s,
               "P10.4 a later launch loads the stored file");
        expect(vfs.write_text(performance_settings_vfs_path(), "{not json"),
               "P10.4 corrupt the settings file");
        expect(ensure_first_launch_settings(vfs, again, &created, &err) && created &&
                   again.quality_preset == QualityPreset::Medium,
               "P10.4 a corrupt file is replaced with the first-launch defaults");
    }

    {
        MemoryVfs vfs;
        PerformanceSettings stored{};
        stored.frame_rate_cap = FrameRateCap::Fps90;
        std::string err;
        expect(save_performance_settings(vfs, stored, &err), "P10.4 seed stored settings");
        PerformanceSettings merged{};
        expect(save_launcher_settings_json(
                   vfs, "{\"ui_scale\":\"1.25\",\"language\":\"de\",\"ui_aspect\":\"16:9\"}",
                   &merged, &err) &&
                   merged.ui_scale == 1.25f && merged.language == "de" &&
                   merged.ui_aspect == UiAspectMode::Ratio16x9 &&
                   merged.frame_rate_cap == FrameRateCap::Fps90,
               "P10.4 a partial save changes only the keys it names");
        PerformanceSettings loaded{};
        expect(load_performance_settings(vfs, loaded, &err) && loaded == merged,
               "P10.4 the partial save is on disk");
        expect(!save_launcher_settings_json(vfs, "{\"render_scale\":\"fast\"}", nullptr, &err) &&
                   load_performance_settings(vfs, loaded, &err) && loaded == merged,
               "P10.4 an invalid value saves nothing");
        expect(save_launcher_settings_json(vfs, "{\"quality_preset\":\"low\"}", &merged, &err) &&
                   merged.quality_preset == QualityPreset::Low &&
                   merged.frame_rate_cap == FrameRateCap::Fps30 &&
                   merged.max_texture_size == 512U && merged.ui_scale == 1.25f,
               "P10.4 choosing a preset applies its values and keeps the UI settings");
        expect(save_launcher_settings_json(
                   vfs, "{\"quality_preset\":\"low\",\"frame_rate_cap\":\"60\"}", &merged,
                   &err) &&
                   merged.frame_rate_cap == FrameRateCap::Fps60,
               "P10.4 explicit keys win over the preset in the same save");
    }
}

void test_p8_performance_and_frame_pacing() {
    using namespace wse::platform;

    // 1. FrameRateCap & PresentMode helpers.
    {
        FrameRateCap cap{};
        expect(parse_frame_rate_cap("60", cap) && cap == FrameRateCap::Fps60 &&
                   parse_frame_rate_cap("90Hz", cap) && cap == FrameRateCap::Fps90 &&
                   parse_frame_rate_cap(" 120 ", cap) && cap == FrameRateCap::Fps120 &&
                   parse_frame_rate_cap("uncapped", cap) && cap == FrameRateCap::Uncapped &&
                   parse_frame_rate_cap("0", cap) && cap == FrameRateCap::Uncapped &&
                   !parse_frame_rate_cap("not60", cap),
               "frame rate cap parsing supports 60/90/120/uncapped and rejects junk");

        expect(select_present_mode(FrameRateCap::Uncapped, PresentModePreference::Immediate,
                                   true, true, true) == PresentModePreference::Immediate &&
                   select_present_mode(FrameRateCap::Fps60, PresentModePreference::Immediate,
                                       false, true, true) == PresentModePreference::FifoRelaxed &&
                   select_present_mode(FrameRateCap::Fps90, PresentModePreference::Mailbox,
                                       true, false, false) == PresentModePreference::Mailbox &&
                   sdl_vsync_for_present_mode(PresentModePreference::Fifo, FrameRateCap::Fps60) == 1 &&
                   sdl_vsync_for_present_mode(PresentModePreference::Immediate, FrameRateCap::Uncapped) == 0 &&
                   sdl_vsync_for_present_mode(PresentModePreference::FifoRelaxed, FrameRateCap::Fps60) == -1,
               "present mode negotiation and SDL vsync mapping follow cap & support flags");
    }

    // 2. PerformanceSettings Vfs JSON round-trip and sanitization.
    {
        MemoryVfs vfs;
        PerformanceSettings s{};
        s.frame_rate_cap = FrameRateCap::Fps90;
        s.present_mode = PresentModePreference::Mailbox;
        s.thermal_throttling_enabled = true;
        s.dynamic_resolution_enabled = false;
        s.lod_bias_scaling_enabled = true;
        s.min_resolution_scale = 0.72f;
        s.max_lod_bias = 1.25f;
        std::string err;
        PerformanceSettings loaded{};
        expect(save_performance_settings(vfs, s, &err) &&
                   load_performance_settings(vfs, loaded, &err) &&
                   loaded == s,
               "performance settings save/load round-trips via Vfs");

        PerformanceSettings bad{};
        expect(!deserialize_performance_settings_json("{\"version\":\"9\"}", bad, &err) &&
                   !deserialize_performance_settings_json("{\"frame_rate_cap\":\"144\"}", bad, &err),
               "malformed version or invalid frame rate cap rejected");

        PerformanceSettings dirty{};
        dirty.min_resolution_scale = std::numeric_limits<float>::quiet_NaN();
        dirty.max_lod_bias = 99.0f;
        sanitize_performance_settings(dirty);
        expect(std::fabs(dirty.min_resolution_scale - 0.65f) < 1e-5f &&
                   std::fabs(dirty.max_lod_bias - 3.0f) < 1e-5f,
               "sanitize_performance_settings replaces NaN and clamps bounds");
    }

    // 2b. config/performance.json is the one saved copy of the launcher toggles (P8 review #4).
    {
        MemoryVfs vfs;
        PerformanceSettings s{};
        bool present = true;
        std::string err;
        expect(load_performance_settings_or_default(vfs, s, &present) && !present &&
                   s == PerformanceSettings{},
               "a missing performance.json loads the defaults and reports it absent");

        PerformanceSettings tuned{};
        tuned.min_resolution_scale = 0.8f;
        tuned.max_lod_bias = 0.5f;
        expect(save_performance_settings(vfs, tuned, &err), "seed performance.json");
        LauncherPerformanceChoices c{};
        c.frame_rate_cap = FrameRateCap::Fps120;
        c.present_mode = PresentModePreference::Mailbox;
        c.thermal_throttling_enabled = false;
        c.dynamic_resolution_enabled = false;
        c.lod_bias_scaling_enabled = false;
        c.gyro_aim_enabled = true;
        c.pinch_zoom_enabled = false;
        PerformanceSettings merged{};
        PerformanceSettings back{};
        expect(save_launcher_choices(vfs, c, &merged, &err) &&
                   load_performance_settings_or_default(vfs, back, &present) && present &&
                   back == merged && back.frame_rate_cap == FrameRateCap::Fps120 &&
                   back.present_mode == PresentModePreference::Mailbox &&
                   !back.thermal_throttling_enabled && !back.dynamic_resolution_enabled &&
                   !back.lod_bias_scaling_enabled && back.gyro_aim_enabled &&
                   !back.pinch_zoom_enabled,
               "launcher choices are written into performance.json and read back");
        expect(std::fabs(back.min_resolution_scale - 0.8f) < 1e-5f &&
                   std::fabs(back.max_lod_bias - 0.5f) < 1e-5f,
               "launcher choices keep the JSON-only resolution floor and LOD ceiling");

        PerformanceSettings old{};
        expect(deserialize_performance_settings_json(
                   "{\"version\":\"1\",\"frame_rate_cap\":\"90\"}", old, &err) &&
                   !old.gyro_aim_enabled && old.pinch_zoom_enabled,
               "a file from before the launcher toggles moved loads their defaults");
        expect(!deserialize_performance_settings_json("{\"gyro_aim_enabled\":\"maybe\"}", old,
                                                      &err),
               "an invalid launcher toggle is rejected");

        const std::string junk = "{not json";
        expect(vfs.write_file("config/performance.json",
                              std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>(junk.data()), junk.size())),
               "write a corrupt performance.json");
        PerformanceSettings fallback{};
        fallback.max_lod_bias = 2.0f;
        expect(!load_performance_settings_or_default(vfs, fallback, &present) && present &&
                   fallback == PerformanceSettings{},
               "a corrupt performance.json yields the defaults and false");
        expect(save_launcher_choices(vfs, c, nullptr, &err) &&
                   load_performance_settings(vfs, back, &err) && back.gyro_aim_enabled,
               "saving launcher choices replaces a corrupt performance.json");
    }

    // 3. ThermalPerformanceController: headroom + status + hysteresis + toggles + resolution extent.
    {
        ThermalPerformanceController ctrl;
        PerformanceSettings s{};
        s.frame_rate_cap = FrameRateCap::Fps120;
        s.present_mode = PresentModePreference::Mailbox;
        s.min_resolution_scale = 0.65f;
        s.max_lod_bias = 1.5f;
        ctrl.set_settings(s);
        ctrl.set_present_capabilities(true, true, true);

        ctrl.update_thermal(0, 0.25f);
        auto a = ctrl.evaluate();
        expect(a.tier == ThermalTier::Nominal && std::fabs(a.resolution_scale - 1.0f) < 1e-5f &&
                   std::fabs(a.lod_bias) < 1e-5f && a.effective_fps_cap == 120,
               "nominal thermal leaves 120 Hz, 1.0 scale, 0 LOD bias");

        // Escalate -> Elevated (headroom 0.68 >= 0.65).
        ctrl.update_thermal(0, 0.68f);
        a = ctrl.evaluate();
        expect(a.tier == ThermalTier::Elevated && a.effective_fps_cap == 90 &&
                   std::fabs(a.resolution_scale - 0.90f) < 1e-5f &&
                   std::fabs(a.lod_bias - 0.35f) < 1e-5f,
               "elevated thermal headroom scales down resolution, LOD bias, and 90 Hz cap");

        // Escalate -> Critical (headroom 0.96 >= 0.95) and test downward hysteresis.
        ctrl.update_thermal(0, 0.96f);
        a = ctrl.evaluate();
        expect(a.tier == ThermalTier::Critical && a.effective_fps_cap == 30 &&
                   std::fabs(a.resolution_scale - 0.65f) < 1e-5f &&
                   std::fabs(a.lod_bias - 1.5f) < 1e-5f,
               "critical headroom drops to min resolution scale, max LOD bias, and 30 Hz fallback");

        // Dropping to 0.90 must hold Critical (above exit bound 0.87).
        ctrl.update_thermal(0, 0.90f);
        expect(ctrl.tier() == ThermalTier::Critical,
               "thermal hysteresis avoids flapping near critical boundary");

        // Dropping to 0.85 enters Throttled (60 Hz), and 0.50 restores Nominal.
        ctrl.update_thermal(0, 0.85f);
        expect(ctrl.tier() == ThermalTier::Throttled && ctrl.evaluate().effective_fps_cap == 60,
               "stepping below exit hysteresis reaches Throttled (60 Hz)");

        // OS Severe status alone (with NaN/unavailable headroom) triggers Critical cleanly.
        ctrl.update_thermal(static_cast<std::int32_t>(ThermalStatus::Severe),
                            std::numeric_limits<float>::quiet_NaN());
        expect(!ctrl.thermal().headroom_valid && ctrl.tier() == ThermalTier::Critical,
               "NaN headroom degrades safely while ThermalStatus still drives scaling");

        // Toggles override scaling individually.
        s.dynamic_resolution_enabled = false;
        ctrl.set_settings(s);
        expect(std::fabs(ctrl.evaluate().resolution_scale - 1.0f) < 1e-5f &&
                   ctrl.evaluate().lod_bias > 1.0f,
               "dynamic_resolution_enabled=false keeps 1.0x while keeping LOD/FPS scaling");
        s.thermal_throttling_enabled = false;
        ctrl.set_settings(s);
        expect(ctrl.tier() == ThermalTier::Nominal && ctrl.evaluate().effective_fps_cap == 120,
               "thermal_throttling_enabled=false disables all thermal throttling");

        const ScaledExtent ext = scale_render_extent(1920U, 1080U, 0.80f);
        expect(ext.width == 1536U && ext.height == 864U,
               "scale_render_extent computes expected scaled pixel dimensions");
    }

    // 4. Deterministic FramePacer pacing across 60 / 90 / 120 Hz + Uncapped + hitch reset.
    {
        for (const std::int32_t fps : {60, 90, 120}) {
            ManualClock clock;
            FramePacer pacer(clock);
            pacer.set_target_fps(fps);
            for (int f = 0; f < fps; ++f) {
                pacer.begin_frame();
                clock.advance_ticks(200);  // 2 ms simulated frame work
                clock.advance_ticks(pacer.remaining_wait_ticks());
            }
            const std::int64_t ticks_per_sec = clock.ticks();
            expect(std::llabs(ticks_per_sec - kTicksPerSecond) <= 1,
                   "FramePacer holds drift-free 1.000s cadence over 1 full second");
        }

        ManualClock clock;
        FramePacer pacer(clock);
        pacer.set_target_fps(60);
        pacer.begin_frame();
        clock.advance_ticks(50000);  // 500 ms background pause
        expect(pacer.remaining_wait_ticks() == 0, "stalled frame does not wait");
        pacer.begin_frame();  // resyncs past hitch so next wait is ~1 frame, not 0 burst
        expect(pacer.remaining_wait_ticks() > 1500, "FramePacer resyncs smoothly after long hitch");

        pacer.set_target_fps(0);
        pacer.begin_frame();
        expect(pacer.remaining_wait_ticks() == 0, "Uncapped mode adds zero pacing wait");
    }

    // 5. Review of 9411644: 30 Hz battery-saver cap.
    {
        FrameRateCap cap{};
        PerformanceSettings s{};
        s.frame_rate_cap = FrameRateCap::Fps30;
        PerformanceSettings back{};
        expect(parse_frame_rate_cap("30Hz", cap) && cap == FrameRateCap::Fps30 &&
                   frame_rate_cap_from_hz(30) == FrameRateCap::Fps30 &&
                   frame_rate_cap_from_hz(24) == FrameRateCap::Fps30 &&
                   frame_rate_cap_from_hz(31) == FrameRateCap::Fps60 &&
                   frame_rate_cap_hz(FrameRateCap::Fps30) == 30 &&
                   deserialize_performance_settings_json(serialize_performance_settings_json(s),
                                                         back) &&
                   back.frame_rate_cap == FrameRateCap::Fps30,
               "30 Hz cap parses, maps from Hz, and round-trips");
        ThermalPerformanceController ctrl;
        ctrl.set_settings(s);
        ctrl.update_thermal(0, 0.70f);  // Elevated caps at 90: 30 stays 30.
        expect(ctrl.evaluate().effective_fps_cap == 30, "thermal tiers never raise a 30 Hz cap");
    }

    // 6. Headroom-less readings hold the last valid headroom; recovery is one tier per update.
    {
        ThermalPerformanceController ctrl;
        PerformanceSettings s{};
        s.frame_rate_cap = FrameRateCap::Fps120;
        ctrl.set_settings(s);
        ctrl.update_thermal(0, 0.85f);
        const bool throttled = ctrl.tier() == ThermalTier::Throttled;
        // The status listener (no headroom) fires: this used to drop straight to Elevated.
        ctrl.update_thermal(static_cast<std::int32_t>(ThermalStatus::Light), -1.0f);
        const bool held = ctrl.tier() == ThermalTier::Throttled;
        ctrl.update_thermal(0, std::numeric_limits<float>::quiet_NaN());
        float eff = 0.0f;
        const bool still_held = ctrl.tier() == ThermalTier::Throttled &&
                                ctrl.effective_headroom(eff) && std::fabs(eff - 0.85f) < 1e-6f &&
                                !ctrl.thermal().headroom_valid;
        expect(throttled && held && still_held,
               "status-only / NaN updates keep the last valid headroom instead of reading cool");
        for (std::uint32_t i = 0; i < ThermalPerformanceController::kMaxHeldHeadroomUpdates; ++i) {
            ctrl.update_thermal(0, -1.0f);
        }
        expect(!ctrl.effective_headroom(eff),
               "a held headroom expires after kMaxHeldHeadroomUpdates readings without one");

        ThermalPerformanceController ramp;
        ramp.set_settings(s);
        ramp.update_thermal(0, 0.97f);
        const bool crit = ramp.tier() == ThermalTier::Critical;
        ramp.update_thermal(0, 0.10f);
        const bool t1 = ramp.tier() == ThermalTier::Throttled && ramp.evaluate().effective_fps_cap == 60;
        ramp.update_thermal(0, 0.10f);
        const bool t2 = ramp.tier() == ThermalTier::Elevated && ramp.evaluate().effective_fps_cap == 90;
        ramp.update_thermal(0, 0.10f);
        const bool t3 = ramp.tier() == ThermalTier::Nominal && ramp.evaluate().effective_fps_cap == 120;
        ramp.update_thermal(0, 0.99f);
        expect(crit && t1 && t2 && t3 && ramp.tier() == ThermalTier::Critical,
               "cooling ramps 30 -> 60 -> 90 -> 120 Hz one tier per update; heating is immediate");
    }

    // 6b. Saving to a fresh on-disk Vfs creates `config/` (MemoryVfs hid this:
    // `HostVfs::write_file` needs the parent directory to exist).
    {
        std::error_code ec;
        const std::filesystem::path root =
            std::filesystem::temp_directory_path(ec) / "wse_platform_test_p8_settings";
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
        HostVfs vfs(root);
        PerformanceSettings s{};
        s.frame_rate_cap = FrameRateCap::Fps90;
        PerformanceSettings back{};
        std::string err;
        const bool saved = save_performance_settings(vfs, s, &err);
        const bool loaded = load_performance_settings(vfs, back, &err) && back == s;
        TouchOverlayProfile prof{};
        prof.gyro_aim_enabled = true;
        TouchOverlayProfile prof_back{};
        const bool prof_ok = save_input_profile(vfs, "Native", prof, nullptr, &err) &&
                             load_input_profile(vfs, "Native", prof_back, nullptr, &err) &&
                             prof_back == prof;
        expect(saved && loaded && prof_ok && vfs.is_dir("config") && vfs.is_dir("input_profiles"),
               "performance settings and input profiles save into a fresh HostVfs");
        std::filesystem::remove_all(root, ec);
    }

    // 7. FrameTimeStats: rolling average, 1% low, and pause handling.
    {
        FrameTimeStats st;
        std::int64_t t = 1000;
        st.record_frame(t);
        for (int i = 0; i < 119; ++i) {
            t += 16666667;
            st.record_frame(t);
        }
        t += 50000000;  // one 50 ms hitch
        st.record_frame(t);
        const bool avg_ok = st.sample_count() == 120U && std::fabs(st.average_fps() - 59.016) < 0.01;
        const bool low_ok = std::fabs(st.low_1pct_fps() - 20.0) < 0.01 &&
                            std::fabs(st.worst_frame_ms() - 50.0) < 1e-6;
        t += 2000000000LL;  // 2 s in the background
        st.record_frame(t);
        expect(avg_ok && low_ok && st.sample_count() == 0U,
               "FrameTimeStats reports average / 1% low and restarts after a long pause");
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
    test_saf_vfs_rename();
    test_rhi_and_driver_loader();
    test_rhi_update_buffer_and_first_index();
    test_p5_shader_modules_d3d9_and_material_rhi();
    test_p4_review_fixes();
    test_p5_rhi_review_fixes();
    test_p8_performance_and_frame_pacing();
    test_p10_launcher_settings();
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
