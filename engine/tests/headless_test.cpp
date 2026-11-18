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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

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

// `find_missing_warband_files`: a wrong folder gets every missing file listed at once, the
// module font may come from the module or the install, and `UiGameShell::start` refuses with
// that list before loading anything.
void test_missing_warband_files() {
    wse::platform::MemoryVfs vfs;
    auto missing = wse::platform::find_missing_warband_files(vfs, "", "Modules/Native");
    expect(missing.size() == 1U && missing[0].find("Modules/Native/") == 0U,
           "an absent module folder is reported once, not file by file");

    expect(vfs.write_text("Modules/Native/module.ini", "x") &&
               vfs.write_text("Modules/Native/scripts.txt", "x"),
           "seed a partial module");
    missing = wse::platform::find_missing_warband_files(vfs, "", "Modules/Native");
    const auto has = [&](std::string_view needle) {
        for (const auto& m : missing) {
            if (m.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    };
    expect(missing.size() == 4U && has("troops.txt") && has("menus.txt") &&
               has("Data/font_data.xml") && has("CommonRes"),
           "a partial install lists every missing file");

    expect(vfs.write_text("Modules/Native/troops.txt", "x") &&
               vfs.write_text("Modules/Native/menus.txt", "x") &&
               vfs.write_text("Modules/Native/Data/font_data.xml", "x") &&
               vfs.write_text("CommonRes/core_ui_meshes.brf", "x"),
           "complete the install (module-local font)");
    expect(wse::platform::find_missing_warband_files(vfs, "", "Modules/Native").empty(),
           "a complete install reports nothing missing");
    expect(!wse::platform::find_missing_warband_files(vfs, "Warband", "Warband/Modules/Native").empty(),
           "a root prefix is honoured");

    wse::platform::MemoryVfs empty;
    wse::platform::UiGameShellOptions opts{};
    opts.run.module_dir = "Modules/Native";
    wse::platform::UiGameShell shell;
    std::string err;
    expect(!shell.start(empty, opts, &err) && shell.missing_files().size() == 1U &&
               err.find("not found") != std::string::npos &&
               err.find("Modules/Native/") != std::string::npos && !shell.running(),
           "UiGameShell::start refuses a wrong folder with the missing-file message");
}

// P10.3: the launcher's install check is `find_missing_warband_files` as one JSON object, and
// the language list comes from the install's `languages/*/` folders.
void test_launcher_install_check_json() {
    expect(wse::platform::install_check_json({}) == "{\"ok\":true,\"missing\":[]}",
           "an empty missing list is ok=true with an empty array");
    expect(wse::platform::install_check_json({"Modules/X/scripts.txt", "CommonRes"}) ==
               "{\"ok\":false,\"missing\":[\"Modules/X/scripts.txt\",\"CommonRes\"]}",
           "missing entries keep their order");
    expect(wse::platform::install_check_json({"a\"b\\c\n\x01"}) ==
               "{\"ok\":false,\"missing\":[\"a\\\"b\\\\c\\n\\u0001\"]}",
           "quotes, backslashes and control characters are escaped");

    wse::platform::MemoryVfs vfs;
    expect(vfs.write_text("Modules/Native/module.ini", "x") &&
               vfs.write_text("Modules/Native/scripts.txt", "x"),
           "seed a partial module for the install check");
    const std::string json = wse::platform::install_check_json(
        wse::platform::find_missing_warband_files(vfs, "", "Modules/Native"));
    expect(json.rfind("{\"ok\":false,\"missing\":[", 0) == 0U &&
               json.find("troops.txt") != std::string::npos &&
               json.find("CommonRes") != std::string::npos,
           "the install check of a partial module lists what is missing");
}

void test_list_warband_languages() {
    wse::platform::MemoryVfs none;
    expect(wse::platform::list_warband_languages(none, "") == std::vector<std::string>{"en"},
           "an install without languages/ still offers English");

    wse::platform::MemoryVfs vfs;
    expect(vfs.write_text("languages/zh-CN/game_menus.csv", "x") &&
               vfs.write_text("languages/de/game_menus.csv", "x") &&
               vfs.write_text("languages/en/readme.txt", "x") &&
               vfs.write_text("languages/notes.txt", "x") &&
               vfs.write_text("Game/languages/fr/game_menus.csv", "x"),
           "seed language folders");
    expect(wse::platform::list_warband_languages(vfs, "") ==
               std::vector<std::string>({"en", "de", "zh-CN"}),
           "English first, then the language folders sorted; files are skipped; no duplicate en");
    expect(wse::platform::list_warband_languages(vfs, "Game") ==
               std::vector<std::string>({"en", "fr"}),
           "a root prefix is honoured");
}

// P10.5: a VFS whose reads block until the test opens the gate, standing in for a slow SAF
// tree. Only `read_file` / `read_text` wait; directory queries pass straight through.
class GatedVfs final : public wse::platform::Vfs {
public:
    explicit GatedVfs(wse::platform::Vfs& inner) : inner_(inner) {}

    void open_gate() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }
    [[nodiscard]] std::size_t reads() const noexcept { return reads_.load(); }

    bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        wait_for_gate();
        return inner_.read_file(path, out);
    }
    bool read_text(std::string_view path, std::string& out) override {
        wait_for_gate();
        return inner_.read_text(path, out);
    }
    bool write_file(std::string_view path, std::span<const std::uint8_t> data) override {
        return inner_.write_file(path, data);
    }
    bool remove_file(std::string_view path) override { return inner_.remove_file(path); }
    bool list_dir(std::string_view path, std::vector<wse::platform::VfsEntry>& out) override {
        return inner_.list_dir(path, out);
    }
    bool is_file(std::string_view path) override { return inner_.is_file(path); }
    bool is_dir(std::string_view path) override { return inner_.is_dir(path); }

private:
    void wait_for_gate() {
        ++reads_;
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return open_; });
    }

    wse::platform::Vfs& inner_;
    std::mutex mutex_{};
    std::condition_variable cv_{};
    bool open_ = false;
    std::atomic<std::size_t> reads_{0U};
};

// Polls `pred` every millisecond for up to `seconds`.
template <class Pred>
bool wait_until(Pred pred, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration<double>(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void seed_async_shell_module(wse::platform::MemoryVfs& vfs) {
    seed_minimal_module(vfs, "Modules/AsyncMod", build_minimal_scripts());
    expect(vfs.write_text(
               "Data/font_data.xml",
               "<FontData width=\"64\" height=\"64\" padding=\"0\" font_size=\"32\" "
               "font_scale=\"100\" line_spacing=\"100\"><FontDetails>"
               "<character code=\"65\" u=\"0\" v=\"0\" w=\"16\" h=\"16\" postshift=\"16\"/>"
               "</FontDetails></FontData>\n"),
           "write synthetic font_data.xml for the async shell");
}

wse::platform::UiGameShellOptions async_shell_options() {
    wse::platform::UiGameShellOptions opts{};
    opts.run.module_dir = "Modules/AsyncMod";
    opts.run.load_options.require_core_files = false;
    opts.check_install = false;
    return opts;
}

// `UiGameShell::start_async`: the load runs on a worker thread while the host keeps polling
// (and presenting a loading screen); progress is readable from the host thread; a cancel or a
// destroyed shell stops the worker without hanging.
void test_ui_game_shell_start_async() {
    using wse::platform::UiShellStartState;
    wse::platform::MemoryVfs mem;
    seed_async_shell_module(mem);

    {
        GatedVfs gated(mem);
        wse::platform::UiGameShell shell;
        expect(shell.poll_start() == UiShellStartState::idle, "a new shell is idle");
        expect(shell.start_async(gated, async_shell_options()),
               "start_async returns at once while reads are blocked");
        expect(shell.poll_start() == UiShellStartState::loading && !shell.running(),
               "the shell is loading, not running, while the worker waits");
        expect(!shell.start_async(gated, async_shell_options()),
               "a second start_async while loading is refused");
        expect(wait_until([&] { return gated.reads() > 0U; }, 5.0),
               "the worker reaches the VFS");
        const wse::platform::UiShellStartProgress p = shell.start_progress();
        expect(p.fraction >= 0.0f && p.fraction < 1.0f && !p.stage.empty(),
               "progress is readable from the host thread during the load");
        expect(!shell.frame(1.0f / 60.0f, 800U, 600U) && shell.geometry() == nullptr,
               "frame() does nothing while loading");
        gated.open_gate();
        expect(wait_until([&] { return shell.poll_start() != UiShellStartState::loading; }, 10.0),
               "the load finishes once reads are released");
        expect(shell.poll_start() == UiShellStartState::ready && shell.running() &&
                   shell.start_error().empty(),
               "a finished load leaves the shell ready and running");
        expect(shell.start_progress().fraction == 1.0f, "progress ends at 1");
        expect(shell.frame(1.0f / 60.0f, 800U, 600U), "frames run after the async start");
    }

    {
        GatedVfs gated(mem);
        wse::platform::UiGameShell shell;
        expect(shell.start_async(gated, async_shell_options()), "start a load to cancel");
        expect(wait_until([&] { return gated.reads() > 0U; }, 5.0),
               "the cancelled worker reaches the VFS");
        shell.cancel_start();
        gated.open_gate();
        expect(wait_until([&] { return shell.poll_start() != UiShellStartState::loading; }, 10.0),
               "a cancelled load ends");
        expect(shell.poll_start() == UiShellStartState::failed && !shell.running() &&
                   shell.start_error().find("cancel") != std::string::npos,
               "a cancelled load reports failed with a cancel message");
    }

    {
        GatedVfs gated(mem);  // declared first: outlives the shell
        std::thread opener;
        {
            wse::platform::UiGameShell shell;
            expect(shell.start_async(gated, async_shell_options()), "start a load to abandon");
            expect(wait_until([&] { return gated.reads() > 0U; }, 5.0),
                   "the abandoned worker reaches the VFS");
            opener = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                gated.open_gate();
            });
        }  // the destructor cancels and joins
        opener.join();
        expect(true, "destroying a loading shell joins its worker");
    }

    {
        wse::platform::MemoryVfs no_font;
        seed_minimal_module(no_font, "Modules/AsyncMod", build_minimal_scripts());
        wse::platform::UiGameShell shell;
        expect(shell.start_async(no_font, async_shell_options()), "start a load that fails");
        expect(wait_until([&] { return shell.poll_start() != UiShellStartState::loading; }, 10.0),
               "a failing load ends");
        expect(shell.poll_start() == UiShellStartState::failed &&
                   shell.start_error().find("UI assets") == 0U && !shell.running(),
               "a failing load reports the same error start() gives");
        expect(shell.start_async(mem, async_shell_options()) &&
                   wait_until([&] { return shell.poll_start() != UiShellStartState::loading; },
                              10.0) &&
                   shell.poll_start() == UiShellStartState::ready,
               "the shell can start again after a failed load");
    }
}

// P11.1: Warband's 2D UI is a 4:3 canvas. `compute_ui_viewport` fits it (or 16:9) centred in
// the window with bars; stretch fills the window. The shell renders into that rectangle and
// maps the pointer through it, so a tap on a bar lands outside the UI.
void test_ui_aspect_modes() {
    using wse::platform::UiAspectMode;
    using wse::platform::UiViewportRect;
    using wse::platform::compute_ui_viewport;
    const auto eq = [](UiViewportRect r, std::uint32_t x, std::uint32_t y, std::uint32_t w,
                       std::uint32_t h) {
        return r.x == x && r.y == y && r.width == w && r.height == h;
    };
    expect(eq(compute_ui_viewport(2400U, 1080U, UiAspectMode::Ratio4x3), 480U, 0U, 1440U, 1080U),
           "4:3 on a 20:9 phone is a centred 1440x1080 picture");
    expect(eq(compute_ui_viewport(2400U, 1080U, UiAspectMode::Ratio16x9), 240U, 0U, 1920U, 1080U),
           "16:9 on a 20:9 phone is a centred 1920x1080 picture");
    expect(eq(compute_ui_viewport(2400U, 1080U, UiAspectMode::Stretch), 0U, 0U, 2400U, 1080U),
           "stretch fills the window");
    expect(eq(compute_ui_viewport(800U, 1000U, UiAspectMode::Ratio4x3), 0U, 200U, 800U, 600U),
           "a window taller than 4:3 gets bars above and below");
    expect(eq(compute_ui_viewport(800U, 600U, UiAspectMode::Ratio4x3), 0U, 0U, 800U, 600U),
           "a 4:3 window has no bars");
    expect(eq(compute_ui_viewport(0U, 600U, UiAspectMode::Ratio4x3), 0U, 0U, 0U, 0U),
           "an empty window gives an empty viewport");

    {
        wse::platform::UiInputAccumulator acc;
        acc.set_viewport(compute_ui_viewport(2400U, 1080U, UiAspectMode::Ratio4x3), 2400U, 1080U);
        wse::platform::RawInputEvent ev{};
        ev.type = wse::platform::RawInputEvent::Type::MouseMotion;
        ev.x = 0.5f;
        ev.y = 0.5f;
        acc.feed(ev);
        wse::game::UiPointerInput in = acc.take(0.0f);
        expect(std::abs(in.x - 0.5f * wse::game::kUiScreenWidth) < 1e-4f &&
                   std::abs(in.y - 0.5f * wse::game::kUiScreenHeight) < 1e-4f,
               "the window centre maps to the UI centre");
        ev.x = (480.0f + 1440.0f * 0.25f) / 2400.0f;  // a quarter into the picture
        ev.y = 0.0f;                                  // top edge
        acc.feed(ev);
        in = acc.take(0.0f);
        expect(std::abs(in.x - 0.25f * wse::game::kUiScreenWidth) < 1e-4f &&
                   std::abs(in.y - wse::game::kUiScreenHeight) < 1e-4f,
               "a point inside the picture maps through the viewport");
        ev.x = 0.1f;  // 240 px: inside the left bar
        acc.feed(ev);
        in = acc.take(0.0f);
        expect(in.x < 0.0f, "a point on the left bar lands left of the UI");
        ev.x = 0.95f;  // 2280 px: inside the right bar
        acc.feed(ev);
        in = acc.take(0.0f);
        expect(in.x > wse::game::kUiScreenWidth, "a point on the right bar lands right of the UI");
    }

    wse::platform::MemoryVfs mem;
    seed_async_shell_module(mem);
    {
        wse::platform::UiGameShellOptions opts = async_shell_options();
        expect(opts.ui_aspect == UiAspectMode::Ratio4x3, "the shell defaults to the 4:3 UI");
        opts.output = wse::platform::UiGameShellOptions::Output::geometry;
        wse::platform::UiGameShell shell;
        std::string err;
        expect(shell.start(mem, opts, &err), "start the shell for the aspect test");
        expect(shell.frame(1.0f / 60.0f, 2400U, 1080U) && shell.geometry() != nullptr,
               "the shell records a frame at 2400x1080");
        expect(eq(shell.viewport(), 480U, 0U, 1440U, 1080U),
               "the shell reports the 4:3 viewport it drew into");
        const wse::platform::GeometryFrame* g = shell.geometry();
        bool inside = true;  // the seeded module may draw nothing; ui_test checks placement
        for (const auto& v : g->vertices) {
            inside = inside && v.x >= 480.0f - 0.5f && v.x <= 1920.0f + 0.5f && v.y >= -0.5f &&
                     v.y <= 1080.5f;
        }
        expect(g->width == 2400U && g->height == 1080U && inside,
               "geometry keeps the window size and every vertex lies inside the viewport");
        expect(g->viewport_x == 480U && g->viewport_y == 0U && g->viewport_width == 1440U &&
                   g->viewport_height == 1080U,
               "the geometry frame carries the viewport for the presenter");
    }
    {
        // Module frames (game_start already run) route the pointer into the world's cursor.
        wse::platform::UiGameShellOptions opts = async_shell_options();
        opts.start_at_initial_window = false;
        wse::platform::UiGameShell shell;
        std::string err;
        expect(shell.start(mem, opts, &err), "start the shell in module frames");
        wse::platform::RawInputEvent ev{};
        ev.type = wse::platform::RawInputEvent::Type::MouseMotion;
        ev.x = 0.1f;
        ev.y = 0.5f;
        shell.feed(ev);
        expect(shell.frame(1.0f / 60.0f, 2400U, 1080U) && shell.vm() != nullptr &&
                   shell.vm()->world.m5.cursor_pos.x < 0.0f,
               "a pointer on the bar reaches the UI outside its canvas");
    }
    {
        wse::platform::UiGameShellOptions opts = async_shell_options();
        opts.ui_aspect = UiAspectMode::Stretch;
        opts.output = wse::platform::UiGameShellOptions::Output::geometry;
        wse::platform::UiGameShell shell;
        std::string err;
        expect(shell.start(mem, opts, &err) && shell.frame(1.0f / 60.0f, 2400U, 1080U) &&
                   eq(shell.viewport(), 0U, 0U, 2400U, 1080U),
               "stretch draws over the whole window");
        const std::uint64_t serial = shell.frame_serial();
        expect(shell.frame(1.0f / 60.0f, 2400U, 1080U) && shell.frame_serial() == serial,
               "an unchanged frame is still reused with a viewport");
    }
    {
        wse::platform::UiGameShellOptions opts = async_shell_options();  // software output
        wse::platform::UiGameShell shell;
        std::string err;
        expect(shell.start(mem, opts, &err) && shell.frame(1.0f / 60.0f, 2400U, 1080U) &&
                   shell.width() == 960U && shell.height() == 720U &&
                   eq(shell.viewport(), 480U, 0U, 1440U, 1080U),
               "the CPU frame is the viewport scaled to max_render_height; the viewport is in "
               "window pixels");
    }
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
    opts.check_install = false;  // synthetic module: no full install
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

    // An unchanged tree at the same size keeps the last frame; a changed one redraws.
    const std::uint64_t serial = shell.frame_serial();
    expect(shell.frame(0.0f, 1600U, 1200U) && shell.frame_serial() == serial &&
               shell.frames_reused() == 1U,
           "an unchanged widget tree reuses the last frame");
    {
        wse::game::UiWidget a{};
        a.children.emplace_back();
        a.children[0].children.emplace_back();
        wse::game::UiWidget b = a;
        expect(wse::platform::UiTreeHasher::hash(a) == wse::platform::UiTreeHasher::hash(b),
               "equal trees hash equal");
        b.children[0].children[0].text = "x";
        expect(wse::platform::UiTreeHasher::hash(a) != wse::platform::UiTreeHasher::hash(b),
               "a text change two levels down changes the tree hash");
        b = a;
        b.children[0].children[0].highlighted = true;
        expect(wse::platform::UiTreeHasher::hash(a) != wse::platform::UiTreeHasher::hash(b),
               "a hover-state change changes the tree hash");
    }
    const double before = vm->appTimeSeconds;
    expect(shell.frame(5.0f, 640U, 360U), "long shell frame runs");
    expect(std::abs(vm->appTimeSeconds - before - 0.25) < 1e-9,
           "a 5 s display frame advances the world by max_frame_seconds only");
    expect(shell.width() == 480U && shell.height() == 360U,
           "a window under max_render_height renders its 4:3 viewport at native size");

    shell.stop();
    expect(!shell.running() && !shell.frame(0.1f, 640U, 360U) && shell.pixels().empty(),
           "a stopped shell does not step");

    // Main menu first: no module frames until "Start a New Game"; a game_start that queues
    // no window gets game menu 0.
    seed_minimal_module(vfs, "Modules/MenuMod", build_minimal_scripts());
    wse::platform::UiGameShellOptions menu_opts{};
    menu_opts.run.module_dir = "Modules/MenuMod/";
    menu_opts.run.load_options.require_core_files = false;
    menu_opts.check_install = false;
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
    // Hovering a button changes the tree (highlight), so that frame redraws; the next reuses it.
    const std::uint64_t menu_serial = menu_shell.frame_serial();
    expect(menu_shell.frame(1.0f / 60.0f, 800U, 600U) && menu_shell.frame_serial() == menu_serial,
           "a still main menu reuses its frame");
    {
        wse::platform::RawInputEvent hover{};
        hover.type = wse::platform::RawInputEvent::Type::MouseMotion;
        hover.x = (0.05f + 0.0925f) / wse::game::kUiScreenWidth;
        hover.y = 1.0f - (0.44f + 0.0155f) / wse::game::kUiScreenHeight;
        menu_shell.feed(hover);
    }
    expect(menu_shell.frame(1.0f / 60.0f, 800U, 600U) &&
               menu_shell.frame_serial() == menu_serial + 1U,
           "hovering a main-menu button redraws the frame");
    expect(menu_shell.frame(1.0f / 60.0f, 800U, 600U) &&
               menu_shell.frame_serial() == menu_serial + 1U,
           "the hovered menu, unchanged, reuses its frame");
    // Geometry output: batches at the full window size, no CPU frame, same reuse rule.
    {
        wse::platform::UiGameShellOptions geo_opts = menu_opts;
        geo_opts.output = wse::platform::UiGameShellOptions::Output::geometry;
        wse::platform::UiGameShell geo_shell;
        expect(geo_shell.start(vfs, geo_opts, &err) && geo_shell.geometry() == nullptr &&
                   geo_shell.geometry_device() != nullptr,
               "geometry shell starts with no frame recorded yet");
        expect(geo_shell.frame(1.0f / 60.0f, 2400U, 1080U) && geo_shell.geometry() != nullptr &&
                   geo_shell.geometry()->width == 2400U && geo_shell.geometry()->height == 1080U &&
                   geo_shell.pixels().empty(),
               "geometry output records at the full window size and rasterizes nothing");
        const std::uint64_t s0 = geo_shell.frame_serial();
        expect(geo_shell.frame(1.0f / 60.0f, 2400U, 1080U) && geo_shell.frame_serial() == s0,
               "an unchanged tree keeps the recorded geometry");
    }
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
    test_missing_warband_files();
    test_launcher_install_check_json();
    test_list_warband_languages();
    test_ui_game_shell_start_async();
    test_ui_aspect_modes();

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
