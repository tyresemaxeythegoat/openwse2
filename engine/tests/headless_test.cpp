// Host tests for the Phase 2H clean-room headless execution runner.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

#include "wse/game/headless_runner.h"
#include "wse/platform/log.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"
#include "wse/vm/operand.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

constexpr std::uint64_t op_global(std::uint64_t slot) noexcept {
    return (static_cast<std::uint64_t>(wse::vm::OperandTag::slot) << 56) | slot;
}

constexpr std::uint64_t op_reg(std::uint64_t reg) noexcept {
    return (static_cast<std::uint64_t>(wse::vm::OperandTag::global) << 56) | reg;
}

std::string build_minimal_scripts(std::uint32_t game_start_op = wse::vm::core_op::kAssign) {
    std::ostringstream oss;
    oss << "scriptsfile version 1\n1\n"
        << "game_start -1\n1\n"
        << game_start_op << " 2 " << op_global(1U) << " 7\n";
    return oss.str();
}

std::string build_minimal_triggers() {
    std::ostringstream oss;
    oss << "triggersfile version 1\n1\n"
        << "1.500000 0.000000 0.000000 "
        << "1 " << wse::vm::core_op::kEq << " 2 " << op_global(1U) << " 7 "
        << "1 " << wse::vm::core_op::kValAdd << " 2 " << op_global(2U) << " 10\n";
    return oss.str();
}

std::string build_minimal_simple_triggers() {
    std::ostringstream oss;
    oss << "simple_triggers_file version 1\n1\n"
        << "2.000000 "
        << "2 "
        << wse::vm::core_op::kValAdd << " 2 " << op_global(3U) << " 1 "
        << wse::vm::core_op::kAssign << " 2 " << op_reg(0U) << " " << op_global(2U) << "\n";
    return oss.str();
}

void seed_minimal_module(wse::platform::Vfs& vfs,
                         std::string_view mod_dir,
                         std::string_view scripts_body) {
    const std::string base = wse::game::vfs_join_path(mod_dir, "scripts.txt");
    expect(vfs.write_text(base, scripts_body), "write synthetic scripts.txt");
    expect(vfs.write_text(wse::game::vfs_join_path(mod_dir, "triggers.txt"),
                          build_minimal_triggers()),
           "write synthetic triggers.txt");
    expect(vfs.write_text(
               wse::game::vfs_join_path(mod_dir, "simple_triggers.txt"),
               build_minimal_simple_triggers()),
           "write synthetic simple_triggers.txt");
}

void test_headless_memory_game_start_and_frames() {
    wse::platform::MemoryVfs vfs;
    seed_minimal_module(vfs, "Modules/TestMod", build_minimal_scripts());

    wse::platform::CaptureLog log;
    wse::game::HeadlessRunOptions opts{};
    opts.module_dir = "Modules/TestMod";
    opts.load_options.require_core_files = false;
    opts.trigger_frames = 3;
    opts.frame_ticks = 100000; // 1.0s per frame
    opts.log = &log;

    const auto session = wse::game::run_headless_session(vfs, opts);
    const auto& r = session.report;

    expect(r.ok && r.load_error.ok, "synthetic headless run succeeds");
    expect(r.loaded_files == 3U, "synthetic headless run loads 3 files");
    expect(r.game_start_executed && r.game_start_ok &&
               r.game_start_script_index == 0,
           "game_start executes cleanly at index 0");
    expect(r.frames_executed == 3U && r.triggers_ok && r.simple_triggers_ok,
           "all 3 trigger frames execute cleanly");
    expect(r.error_count == 0U && r.unimplemented_count == 0U &&
               r.unimplemented_opcodes.empty(),
           "zero errors and zero unimplemented opcodes");
    expect(session.ctx->globalVariables.get(1) == 7,
           "game_start sets global var 1");
    expect(session.ctx->globalVariables.get(2) == 10,
           "1.5s trigger fires once at t=2.0s over 3x1.0s frames");
    expect(session.ctx->globalVariables.get(3) == 1,
           "2.0s simple trigger fires once at t=2.0s");
    expect(session.ctx->registers[0] == 10,
           "simple trigger observes value written by trigger");
    expect(log.lines.empty(), "no errors logged on clean run");
}

void test_headless_unimplemented_opcode_fails_loudly() {
    wse::platform::MemoryVfs vfs;
    seed_minimal_module(vfs, "Modules/BadOpMod", build_minimal_scripts(9999U));

    wse::platform::CaptureLog log;
    wse::game::HeadlessRunOptions opts{};
    opts.module_dir = "Modules/BadOpMod";
    opts.load_options.require_core_files = false;
    opts.trigger_frames = 2;
    opts.fail_on_unimplemented = true;
    opts.log = &log;

    const auto session = wse::game::run_headless_session(vfs, opts);
    const auto& r = session.report;

    expect(!r.ok, "unimplemented opcode fails headless run");
    expect(r.game_start_executed && !r.game_start_ok,
           "game_start aborts on unimplemented opcode 9999");
    expect(r.frames_executed == 0U,
           "stop_on_error halts before stepping frames after failed game_start");
    expect(r.unimplemented_count == 1U && r.unimplemented_opcodes.size() == 1U &&
               r.unimplemented_opcodes[0].first == 9999U &&
               r.unimplemented_opcodes[0].second == 1U,
           "unimplemented opcode histogram records 9999 x 1");
    expect(r.error_count >= 1U && !log.lines.empty(),
           "loud error recorded and forwarded to LogSink");
}

void test_headless_allow_unimplemented_continues_and_reports() {
    wse::platform::MemoryVfs vfs;
    seed_minimal_module(vfs, "Modules/BadOpMod", build_minimal_scripts(9999U));

    wse::platform::CaptureLog log;
    wse::game::HeadlessRunOptions opts{};
    opts.module_dir = "Modules/BadOpMod";
    opts.load_options.require_core_files = false;
    opts.trigger_frames = 2;
    opts.fail_on_unimplemented = false;
    opts.log = &log;

    const auto session = wse::game::run_headless_session(vfs, opts);
    const auto& r = session.report;

    expect(r.game_start_executed && r.game_start_ok,
           "compat mode: game_start falls through the unimplemented opcode");
    expect(r.frames_executed == 2U && r.triggers_ok && r.simple_triggers_ok,
           "compat mode: trigger frames still run after an unimplemented opcode");
    expect(r.unimplemented_count == 1U && r.unimplemented_opcodes.size() == 1U &&
               r.unimplemented_opcodes[0].first == 9999U,
           "compat mode: unimplemented opcode is still counted and reported");
    expect(r.ok && r.error_count == 0U,
           "compat mode: unimplemented opcodes alone do not fail the run");
}

void test_headless_missing_game_start_and_load_failure() {
    wse::platform::MemoryVfs vfs;
    expect(vfs.write_text("Modules/NoStart/scripts.txt",
                          "scriptsfile version 1\n1\nother_script -1\n0\n"),
           "write scripts without game_start");

    wse::game::HeadlessRunOptions opts{};
    opts.module_dir = "Modules/NoStart";
    opts.load_options.require_core_files = false;
    opts.require_game_start = true;
    const auto strict_res = wse::game::run_headless_session(vfs, opts);
    expect(!strict_res.report.ok &&
               strict_res.report.game_start_script_index == -1 &&
               strict_res.report.error_count >= 1U,
           "require_game_start fails when game_start is unmapped");

    opts.require_game_start = false;
    auto opt_ctx = std::make_unique<wse::vm::VmContext>();
    auto opt_bundle = std::make_unique<wse::game::ModuleBundle>();
    const auto relaxed =
        wse::game::run_headless_module(vfs, *opt_ctx, *opt_bundle, opts);
    expect(relaxed.ok && !relaxed.game_start_executed &&
               relaxed.frames_executed == 1U,
           "optional game_start still steps frames");

    wse::game::HeadlessRunOptions missing_opts{};
    missing_opts.module_dir = "Modules/Missing";
    missing_opts.load_options.require_core_files = true;
    const auto load_fail = wse::game::run_headless_session(vfs, missing_opts);
    expect(!load_fail.report.ok && !load_fail.report.load_error.ok,
           "missing required module reports load failure");
}

void test_headless_host_vfs_roundtrip() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / "wse_headless_host_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "Modules" / "TestHost", ec);
    expect(!ec, "create host temp directory");

    {
        wse::platform::HostVfs host(root);
        seed_minimal_module(host, "Modules/TestHost", build_minimal_scripts());

        wse::platform::CaptureLog log;
        wse::game::HeadlessRunOptions opts{};
        opts.module_dir = "Modules/TestHost";
        opts.load_options.require_core_files = false;
        opts.trigger_frames = 2;
        opts.frame_ticks = 100000;
        opts.log = &log;

        const auto session = wse::game::run_headless_session(host, opts);
        expect(session.report.ok &&
                   session.ctx->globalVariables.get(1) == 7 &&
                   session.ctx->globalVariables.get(2) == 10 &&
                   session.ctx->globalVariables.get(3) == 1,
               "HostVfs headless run executes game_start and trigger frames");
    }

    fs::remove_all(root, ec);
}

} // namespace

namespace wse::selftest {

int run_headless_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_headless_memory_game_start_and_frames();
    test_headless_unimplemented_opcode_fails_loudly();
    test_headless_allow_unimplemented_continues_and_reports();
    test_headless_missing_game_start_and_load_failure();
    test_headless_host_vfs_roundtrip();

    if (failures == 0) {
        std::printf("headless tests: PASS\n");
        return 0;
    }
    std::printf("headless tests: FAIL (%d)\n", failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_headless_suite(nullptr);
}
#endif
