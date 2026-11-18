// Interactive module shell for app frame loops (SDL / Android): loads a module and its UI
// assets once, then each display frame feeds the platform input through
// `UiInputAccumulator`, steps the module (`game::HeadlessModuleRunner::step`, which hands the
// pointer to `UiSession::handle_input` before `drive_frame`) and draws `UiSession::root` with
// `UiRhiRenderer` into an RGBA8 frame the host uploads to its window.
//
// The shell opens on the main menu (`UiSession::build_initial_window`, `mbInitialWindow`):
// no module frames run there. "Start a New Game" runs `game_start`
// (`HeadlessModuleRunner::run_game_start`); when that queues no window, the shell opens game
// menu 0 the way `jump_to_menu` does (Module System convention: menu 0, `start_game_0` in
// Native, is the new-game menu). Port choice: retail tears `mbGame` down and rebuilds it
// through the starting / loading windows before the first menu; that sequence is not traced.
//
// Port infrastructure (no single retail counterpart): retail runs the same order inside
// `rglWindowManager::frameMove` (`0x14032a0d0`). The world model is the headless one (campaign
// map, no mission scene), so what the shell shows is the module's menus, presentations and
// notes windows; the frame itself goes through `SoftwareRhiDevice` until the Vulkan device
// draws (`STATUS.md` §5, P4 item 1).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/game/headless_runner.h"
#include "wse/platform/input.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_ui.h"
#include "wse/platform/ui_frame.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

struct UiGameShellOptions {
    // Module run settings. `drive_ui` is forced on; `trigger_frames` is unused (the host
    // decides how many frames run). Unimplemented opcodes are counted, not fatal, by default:
    // an interactive session should keep going where a validation run would stop.
    game::HeadlessRunOptions run{};
    // Warband root inside the VFS (`""` when the VFS is rooted at the install) and the
    // language for `languages/<lang>/*.csv`.
    std::string warband_root{};
    std::string language = "en";
    // Largest dimension of the rendered frame; the host scales the frame to its window. The
    // software rasterizer's cost grows with the pixel count, so phones render below native.
    std::uint32_t max_render_height = 720U;
    // A display frame longer than this (a stall, or the app returning from the background)
    // advances the world by this much only.
    float max_frame_seconds = 0.25f;
    // Open on the main menu and defer `game_start` to "Start a New Game". Off: `game_start`
    // runs in `start` (`run.run_game_start`) and module frames run from the first frame.
    bool start_at_initial_window = true;
    UiRenderConfig render{};

    UiGameShellOptions() { run.fail_on_unimplemented = false; }
};

class UiGameShell {
public:
    UiGameShell() = default;
    UiGameShell(const UiGameShell&) = delete;
    UiGameShell& operator=(const UiGameShell&) = delete;
    ~UiGameShell() { stop(); }

    // Loads the UI assets and the module and runs `game_start`. `vfs` must outlive the shell.
    // False with `error_out` set when the font / UI resources or the module fail to load, or
    // `game_start` stops the run.
    [[nodiscard]] bool start(Vfs& vfs, UiGameShellOptions options, std::string* error_out = nullptr) {
        stop();
        options_ = std::move(options);
        options_.run.drive_ui = true;

        assets_ = std::make_unique<LoadedUiAssets>();
        std::string err;
        if (!load_native_ui_assets(vfs, options_.warband_root, options_.run.module_dir,
                                   options_.language, *assets_, &err)) {
            return fail("UI assets: " + err, error_out);
        }
        rhi_ = std::make_unique<SoftwareRhiDevice>();
        renderer_ = std::make_unique<UiRhiRenderer>(*rhi_);
        if (!renderer_->init(assets_.get(), &err)) {
            return fail("UI renderer: " + err, error_out);
        }

        ctx_ = std::make_unique<vm::VmContext>();
        bundle_ = std::make_unique<game::ModuleBundle>();
        ui_ = std::make_unique<game::UiSession>();
        ui_->font = &assets_->font;
        ui_->lang = assets_->lang;
        ui_->vars = assets_->vars;
        ui_->mesh_bounds = collect_ui_mesh_bounds(*assets_);
        game::HeadlessRunOptions run = options_.run;
        if (options_.start_at_initial_window) {
            run.run_game_start = false;
        }
        runner_ = std::make_unique<game::HeadlessModuleRunner>(vfs, *ctx_, *bundle_, run,
                                                               ui_.get());
        if (!runner_->start()) {
            const auto& rep = runner_->report();
            return fail(rep.load_error.ok ? "game_start: " + rep.last_error
                                          : "module: " + rep.load_error.format(),
                        error_out);
        }
        input_ = UiInputAccumulator{};
        quit_requested_ = false;
        in_game_ = !options_.start_at_initial_window;
        if (!in_game_) {
            ui_->build_initial_window(ctx_->world, module_display_name(options_.run.module_dir),
                                      /*has_saves=*/false, /*can_new_game=*/true,
                                      /*can_tutorial=*/false, /*can_multiplayer=*/false);
        }
        return true;
    }

    void stop() noexcept {
        runner_.reset();  // before the session, VM and bundle it points into
        ui_.reset();
        bundle_.reset();
        ctx_.reset();
        renderer_.reset();  // before the device that owns its handles
        rhi_.reset();
        assets_.reset();
        pixels_.clear();
        width_ = height_ = 0U;
    }

    void feed(const RawInputEvent& ev) { input_.feed(ev); }
    void feed_text(std::string_view utf8) { input_.feed_text(utf8); }

    // Advances the module by `dt_s` (clamped to `[0, max_frame_seconds]`) with the input fed
    // since the last frame, then renders at the window's aspect ratio (`window_w x window_h`,
    // scaled down to `max_render_height`). Returns whether the module is still running; the
    // last frame stays in `pixels()` either way.
    bool frame(float dt_s, std::uint32_t window_w, std::uint32_t window_h) {
        if (runner_ == nullptr) {
            return false;
        }
        const float dt = std::isfinite(dt_s) ? std::clamp(dt_s, 0.0f, options_.max_frame_seconds)
                                             : 0.0f;
        const auto ticks =
            static_cast<std::int64_t>(std::llround(static_cast<double>(dt) * game::kTimerTicksPerSecond));
        const game::UiPointerInput in = input_.take(static_cast<float>(ctx_->appTimeSeconds + dt));
        last_events_.clear();
        bool running = true;
        if (in_game_) {
            running = runner_->step(ticks, &in, &last_events_);
        } else {
            running = main_menu_frame(in);
        }
        render(window_w, window_h);
        return running;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<game::UiEvent>& last_events() const noexcept { return last_events_; }
    [[nodiscard]] bool running() const noexcept { return runner_ != nullptr && runner_->running(); }
    // The main menu's quit button was pressed; the host should close.
    [[nodiscard]] bool quit_requested() const noexcept { return quit_requested_; }
    // `game_start` has run and module frames are stepping.
    [[nodiscard]] bool in_game() const noexcept { return in_game_; }
    [[nodiscard]] game::HeadlessRunReport report() const {
        return runner_ != nullptr ? runner_->report() : game::HeadlessRunReport{};
    }
    [[nodiscard]] game::UiSession* session() const noexcept { return ui_.get(); }
    [[nodiscard]] vm::VmContext* vm() const noexcept { return ctx_.get(); }

private:
    bool fail(std::string message, std::string* error_out) {
        if (error_out != nullptr) {
            *error_out = std::move(message);
        }
        stop();
        return false;
    }

    // `Modules/<name>` -> `<name>` for the main menu's module label.
    [[nodiscard]] static std::string module_display_name(std::string_view module_dir) {
        while (!module_dir.empty() && (module_dir.back() == '/' || module_dir.back() == '\\')) {
            module_dir.remove_suffix(1);
        }
        const std::size_t slash = module_dir.find_last_of("/\\");
        return std::string(slash == std::string_view::npos ? module_dir
                                                           : module_dir.substr(slash + 1U));
    }

    // Main menu: input only (no module frames run before a game exists).
    bool main_menu_frame(const game::UiPointerInput& in) {
        last_events_ = ui_->handle_input(ctx_->world, in, ctx_.get(), bundle_->presentations);
        for (const game::UiEvent& ev : last_events_) {
            if (ev.kind == game::UiActionKind::Quit) {
                quit_requested_ = true;
            } else if (ev.kind == game::UiActionKind::StartNewGame) {
                return start_new_game();
            }
        }
        return true;
    }

    bool start_new_game() {
        game::WseWorldState& world = ctx_->world;
        if (!runner_->run_game_start()) {
            return false;
        }
        in_game_ = true;
        ui_->root = game::UiWidget{};
        if (world.m5.enqueued_windows.empty()) {
            world.jump_menu_no = 0;
            world.enqueue_window(game::kQueuedWindowMenuType);
        }
        return true;
    }

    void render(std::uint32_t window_w, std::uint32_t window_h) {
        if (window_w == 0U || window_h == 0U) {
            return;
        }
        std::uint32_t w = window_w;
        std::uint32_t h = window_h;
        if (options_.max_render_height != 0U && h > options_.max_render_height) {
            w = std::max<std::uint32_t>(
                1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(w) *
                                               options_.max_render_height / h));
            h = options_.max_render_height;
        }
        UiRenderConfig cfg = options_.render;
        cfg.viewport_width = w;
        cfg.viewport_height = h;
        if (renderer_->render_tree(ui_->root, assets_->font, cfg, pixels_)) {
            width_ = w;
            height_ = h;
        }
    }

    UiGameShellOptions options_{};
    std::unique_ptr<LoadedUiAssets> assets_{};
    std::unique_ptr<SoftwareRhiDevice> rhi_{};
    std::unique_ptr<UiRhiRenderer> renderer_{};
    std::unique_ptr<vm::VmContext> ctx_{};
    std::unique_ptr<game::ModuleBundle> bundle_{};
    std::unique_ptr<game::UiSession> ui_{};
    std::unique_ptr<game::HeadlessModuleRunner> runner_{};
    UiInputAccumulator input_{};
    std::vector<game::UiEvent> last_events_{};
    std::vector<std::uint8_t> pixels_{};
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    bool in_game_ = false;
    bool quit_requested_ = false;
};

}  // namespace wse::platform
