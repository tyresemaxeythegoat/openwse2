// Host tests for the platform seam (Vfs, Clock, LogSink).
//
// Build/run: see AGENTS.md section 2 (CMake/CTest or the shell loop).

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "wse/io/text_stream.h"
#include "wse/platform/clock.h"
#include "wse/platform/log.h"
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

}  // namespace

int main() {
    test_paths();
    test_memory_vfs();
    test_host_vfs();
    test_vfs_feeds_text_stream();
    test_clock();
    test_log();
    if (failures != 0) {
        std::printf("platform tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("platform tests: PASS\n");
    return 0;
}
