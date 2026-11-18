// Host tests for the Phase 2H clean-room headless execution runner.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

#include "wse/game/headless_runner.h"
#include "wse/platform/ui_game_shell.h"
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

// `game_start` runs `start_presentation 0`; the presentation's -60 sets a 0.5 s duration. With
// `drive_ui` the first frame opens it and the second closes it again (both entry points: the
// session-owned `UiSession` and the run-local one `wse_headless` relies on).
void test_headless_drives_presentation_window() {
    constexpr std::uint32_t kStartPresentation = 900U;
    constexpr std::uint32_t kPresentationSetDuration = 902U;
    wse::platform::MemoryVfs vfs;
    std::ostringstream scripts;
    scripts << "scriptsfile version 1\n1\n"
            << "game_start -1\n1\n"
            << kStartPresentation << " 1 0\n";
    seed_minimal_module(vfs, "Modules/PrsntMod", scripts.str());
    std::ostringstream prsnt;
    prsnt << "presentationsfile version 1\n 1\n"
          << "prsnt_headless 0 0 1\n"
          << "-60.000000  1 " << kPresentationSetDuration << " 1 50\n";
    expect(vfs.write_text("Modules/PrsntMod/presentations.txt", prsnt.str()),
           "write synthetic presentations.txt");

    wse::game::HeadlessRunOptions opts{};
    opts.module_dir = "Modules/PrsntMod";
    opts.load_options.require_core_files = false;
    opts.frame_ticks = 100000;  // 1.0 s per frame

    opts.trigger_frames = 1;
    const auto one = wse::game::run_headless_session(vfs, opts);
    expect(one.report.ok && one.report.presentations_opened == 1U &&
               one.report.final_window == wse::game::kScreenWindowPresentation &&
               one.ctx->world.active_presentation_no == 0 &&
               one.ctx->world.m5.enqueued_windows.empty(),
           "headless frame 1 opens the presentation queued by game_start");

    opts.trigger_frames = 2;
    const auto two = wse::game::run_headless_session(vfs, opts);
    expect(two.report.ok && two.report.presentations_opened == 1U &&
               two.report.final_window == wse::game::kScreenWindowInitial &&
               two.ctx->world.active_presentation_no == -1,
           "headless frame 2 closes the presentation back to the window it opened over");

    auto ctx = std::make_unique<wse::vm::VmContext>();
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    const auto direct = wse::game::run_headless_module(vfs, *ctx, *bundle, opts);
    expect(direct.ok && direct.presentations_opened == 1U &&
               direct.final_window == wse::game::kScreenWindowInitial,
           "run_headless_module without a UiSession drives a run-local one");

    opts.drive_ui = false;
    const auto no_ui = wse::game::run_headless_session(vfs, opts);
    expect(no_ui.report.ok && no_ui.report.presentations_opened == 0U && no_ui.ui == nullptr,
           "drive_ui = false opens no window");
}

// `drive_audio` runs the world audio bridge and pumps the mixer each frame, so the mixer
// reports stereo frames even with no sounds; without it nothing is mixed.
void test_headless_drives_audio_mixer() {
    wse::platform::MemoryVfs vfs;
    seed_minimal_module(vfs, "Modules/AudioMod", build_minimal_scripts());

    wse::game::HeadlessRunOptions opts{};
    opts.module_dir = "Modules/AudioMod";
    opts.load_options.require_core_files = false;
    opts.trigger_frames = 3;
    opts.frame_ticks = 100000;  // 1.0 s per frame

    opts.drive_audio = true;
    const auto on = wse::game::run_headless_session(vfs, opts);
    expect(on.report.ok && on.report.frames_executed == 3U,
           "drive_audio run completes 3 frames");
    expect(on.report.audio_frames_mixed > 0,
           "drive_audio mixes stereo frames each frame (silence still counts)");

    opts.drive_audio = false;
    const auto off = wse::game::run_headless_session(vfs, opts);
    expect(off.report.ok && off.report.audio_frames_mixed == 0,
           "drive_audio = false mixes no audio frames");
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

// `HeadlessModuleRunner` stepped by hand gives the same state as `run_headless_module`'s loop,
// and refuses to step before `start` or after a load failure.
void test_headless_module_runner_steps() {
    wse::platform::MemoryVfs vfs;
    seed_minimal_module(vfs, "Modules/TestMod", build_minimal_scripts());
    wse::game::HeadlessRunOptions opts{};
    opts.module_dir = "Modules/TestMod";
    opts.load_options.require_core_files = false;
    opts.trigger_frames = 3;
    opts.frame_ticks = 100000;
    const auto looped = wse::game::run_headless_session(vfs, opts);

    auto ctx = std::make_unique<wse::vm::VmContext>();
    auto bundle = std::make_unique<wse::game::ModuleBundle>();
    wse::game::HeadlessModuleRunner runner(vfs, *ctx, *bundle, opts);
    expect(!runner.step(100000) && !runner.running(), "runner does not step before start");
    expect(runner.start() && runner.running() && ctx->globalVariables.get(1) == 7,
           "runner start loads the module and runs game_start");
    // Uneven steps that add up to the same 3.0 s.
    expect(runner.step(50000) && runner.step(150000) && runner.step(100000),
           "runner steps three frames");
    const auto& rep = runner.finalize();
    expect(rep.ok && rep.frames_executed == 3U && runner.ui() != nullptr,
           "stepped runner reports 3 clean frames with a runner-owned UiSession");
    expect(ctx->globalVariables.get(2) == looped.ctx->globalVariables.get(2) &&
               ctx->globalVariables.get(3) == looped.ctx->globalVariables.get(3) &&
               ctx->registers[0] == looped.ctx->registers[0],
           "stepped runner ends in the same state as run_headless_module's loop");

    // An empty module directory loads no files, so the required game_start is missing.
    wse::game::HeadlessRunOptions missing = opts;
    missing.module_dir = "Modules/Nope";
    auto ctx2 = std::make_unique<wse::vm::VmContext>();
    auto bundle2 = std::make_unique<wse::game::ModuleBundle>();
    wse::game::HeadlessModuleRunner bad(vfs, *ctx2, *bundle2, missing);
    expect(!bad.start() && !bad.running() && !bad.report().ok && !bad.step(100000),
           "runner without game_start fails start and never steps");
}

// `UiGameShell`: loads the font and module, opens the presentation `game_start` queues, routes
// pointer events into `UiSession::handle_input`, clamps long frames, and renders at the
// window's aspect ratio capped at `max_render_height`.
void test_ui_game_shell() {
    constexpr std::uint32_t kStartPresentation = 900U;
    wse::platform::MemoryVfs vfs;
    std::ostringstream scripts;
    scripts << "scriptsfile version 1\n1\n"
            << "game_start -1\n1\n"
            << kStartPresentation << " 1 0\n";
    seed_minimal_module(vfs, "Modules/ShellMod", scripts.str());
    expect(vfs.write_text("Modules/ShellMod/presentations.txt",
                          "presentationsfile version 1\n 1\nprsnt_shell 0 0 0\n"),
           "write synthetic presentations.txt");

    wse::platform::UiGameShellOptions opts{};
    opts.run.module_dir = "Modules/ShellMod";
    opts.run.load_options.require_core_files = false;
    opts.render.clear_r = 1;
    opts.render.clear_g = 2;
    opts.render.clear_b = 3;
    opts.start_at_initial_window = false;  // game_start in start(); the main menu is below

    wse::platform::UiGameShell shell;
    std::string err;
    expect(!shell.start(vfs, opts, &err) && err.find("UI assets") == 0U && !shell.running(),
           "shell start without Data/font_data.xml fails with a UI-asset error");

    expect(vfs.write_text(
               "Data/font_data.xml",
               "<FontData width=\"64\" height=\"64\" padding=\"0\" font_size=\"32\" "
               "font_scale=\"100\" line_spacing=\"100\"><FontDetails>"
               "<character code=\"65\" u=\"0\" v=\"0\" w=\"16\" h=\"16\" postshift=\"16\"/>"
               "</FontDetails></FontData>\n"),
           "write synthetic font_data.xml");
    err.clear();
    expect(shell.start(vfs, opts, &err) && shell.running(), "shell starts the synthetic module");
    if (!err.empty()) {
        std::printf("      shell start error: %s\n", err.c_str());
    }
    if (!shell.running()) {
        return;
    }

    wse::platform::RawInputEvent move{};
    move.type = wse::platform::RawInputEvent::Type::MouseMotion;
    move.x = 0.25f;
    move.y = 0.5f;  // window y down -> UI y = 0.5 * 0.75
    shell.feed(move);
    expect(shell.frame(0.1f, 1600U, 1200U), "first shell frame runs");
    const auto* vm = shell.vm();
    expect(shell.report().presentations_opened == 1U &&
               vm->world.m5.active_window == wse::game::kScreenWindowPresentation,
           "first shell frame opens the presentation game_start queued");
    expect(std::abs(vm->world.m5.cursor_pos.x - 0.25f) < 1e-5f &&
               std::abs(vm->world.m5.cursor_pos.y - 0.375f) < 1e-5f,
           "shell routes the pointer into UiSession::handle_input in UI space");
    expect(shell.width() == 960U && shell.height() == 720U &&
               shell.pixels().size() == 960U * 720U * 4U,
           "1600x1200 window renders at 960x720 (max_render_height)");
    expect(!shell.pixels().empty() && shell.pixels()[0] == 1U && shell.pixels()[1] == 2U &&
               shell.pixels()[2] == 3U,
           "empty presentation renders the configured clear colour");

    const double before = vm->appTimeSeconds;
    expect(shell.frame(5.0f, 640U, 360U), "long shell frame runs");
    expect(std::abs(vm->appTimeSeconds - before - 0.25) < 1e-9,
           "a 5 s display frame advances the world by max_frame_seconds only");
    expect(shell.width() == 640U && shell.height() == 360U,
           "a window under max_render_height renders at native size");

    shell.stop();
    expect(!shell.running() && !shell.frame(0.1f, 640U, 360U) && shell.pixels().empty(),
           "a stopped shell does not step");

    // Main menu first: no module frames until "Start a New Game"; a game_start that queues
    // no window gets game menu 0.
    seed_minimal_module(vfs, "Modules/MenuMod", build_minimal_scripts());
    wse::platform::UiGameShellOptions menu_opts{};
    menu_opts.run.module_dir = "Modules/MenuMod/";
    menu_opts.run.load_options.require_core_files = false;
    wse::platform::UiGameShell menu_shell;
    expect(menu_shell.start(vfs, menu_opts, &err) && !menu_shell.in_game() &&
               menu_shell.session()->root.id == "initial" &&
               menu_shell.session()->module_name == "MenuMod" &&
               menu_shell.vm()->globalVariables.get(1) == 0,
           "shell opens on the main menu, labelled with the module name, before game_start");
    expect(menu_shell.frame(1.0f / 60.0f, 800U, 600U) && !menu_shell.in_game() &&
               menu_shell.report().frames_executed == 0U && menu_shell.vm()->appTimeSeconds == 0.0,
           "main-menu frames run no module frames");
    const auto tap = [&](float ui_x, float ui_y) {
        wse::platform::RawInputEvent e{};
        e.raw_key = wse::platform::RawKey::kLeftMouse;
        e.x = ui_x / wse::game::kUiScreenWidth;
        e.y = 1.0f - ui_y / wse::game::kUiScreenHeight;
        e.type = wse::platform::RawInputEvent::Type::MouseButtonDown;
        menu_shell.feed(e);
        e.type = wse::platform::RawInputEvent::Type::MouseButtonUp;
        menu_shell.feed(e);
    };
    tap(0.05f + 0.0925f, 0.44f + 0.0155f);  // start_new_game_button
    expect(menu_shell.frame(1.0f / 60.0f, 800U, 600U) && menu_shell.in_game() &&
               menu_shell.vm()->globalVariables.get(1) == 7,
           "Start a New Game runs game_start");
    const auto& queued = menu_shell.vm()->world.m5.enqueued_windows;
    expect(menu_shell.vm()->world.jump_menu_no == 0 && !queued.empty() &&
               queued.back().first == wse::game::kQueuedWindowMenuType,
           "a game_start that queues no window opens game menu 0");
    expect(menu_shell.frame(1.0f / 60.0f, 800U, 600U) && menu_shell.report().frames_executed == 1U,
           "module frames run after the new game starts");
}

} // namespace

namespace wse::selftest {

int run_headless_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_headless_memory_game_start_and_frames();
    test_headless_drives_presentation_window();
    test_headless_drives_audio_mixer();
    test_headless_unimplemented_opcode_fails_loudly();
    test_headless_allow_unimplemented_continues_and_reports();
    test_headless_missing_game_start_and_load_failure();
    test_headless_host_vfs_roundtrip();
    test_headless_module_runner_steps();
    test_ui_game_shell();

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
