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
#include "wse/platform/rhi_geometry.h"
#include "wse/platform/rhi_ui.h"
#include "wse/platform/ui_frame.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

// FNV-1a over every field of a widget tree, for skipping the rasterizer when nothing changed:
// `UiRhiRenderer::render_tree` reads only the tree, the font, the render config and the
// uploaded assets, so equal hashes (with equal config) give an identical frame. Every
// `UiWidget` field is hashed, render-relevant or not; a field added to `UiWidget` must be added
// here, or changes to it alone will not redraw.
class UiTreeHasher {
public:
    [[nodiscard]] static std::uint64_t hash(const game::UiWidget& root) {
        UiTreeHasher h;
        h.widget(root);
        return h.state_;
    }

private:
    void bytes(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            state_ = (state_ ^ p[i]) * 0x100000001b3ULL;
        }
    }
    template <typename T>
    void pod(const T& v) noexcept {
        bytes(&v, sizeof(T));
    }
    void str(const std::string& s) noexcept {
        pod(s.size());
        bytes(s.data(), s.size());
    }
    void vec2(const core::WseVec2& v) noexcept {
        pod(v.x);
        pod(v.y);
    }
    void widget(const game::UiWidget& w) {
        str(w.id);
        pod(w.kind);
        vec2(w.pos);
        vec2(w.size);
        str(w.text);
        vec2(w.text_pos);
        vec2(w.text_scale);
        pod(w.text_flags);
        pod(w.color_argb);
        pod(w.tint_mesh);
        pod(w.text_color_argb);
        pod(w.highlight_color_argb);
        str(w.mesh_name);
        str(w.highlight_mesh_name);
        str(w.material_name);
        pod(w.overlay_id);
        pod(w.action_tag);
        pod(w.action_param);
        pod(w.draw_priority);
        pod(w.visible);
        pod(w.enabled);
        pod(w.highlighted);
        pod(w.pressed);
        vec2(w.child_offset);
        pod(w.clip_children);
        pod(w.has_mesh_rect);
        vec2(w.mesh_pos);
        vec2(w.mesh_size);
        pod(w.children.size());
        for (const game::UiWidget& c : w.children) {
            widget(c);
        }
    }

    std::uint64_t state_ = 0xcbf29ce484222325ULL;
};

// The files a module launch cannot do without, checked before anything loads so a wrong
// folder (the user picked `Modules/`, a module folder, or an unrelated directory instead of
// the Warband install) gets one message naming what is missing instead of the first loader
// error. Paths are relative to the VFS root; empty when the install looks complete. Not a full
// validation: a damaged file still fails later with the loader's own error.
[[nodiscard]] inline std::vector<std::string> find_missing_warband_files(
    Vfs& vfs, std::string_view warband_root, std::string_view module_dir) {
    std::vector<std::string> missing;
    const auto root_path = [&](std::string_view rel) {
        return warband_root.empty() ? std::string(rel) : game::vfs_join_path(warband_root, rel);
    };
    if (!vfs.is_dir(module_dir)) {
        missing.push_back(std::string(module_dir) + "/ (module folder)");
        return missing;  // every file inside it would be listed too
    }
    for (const std::string_view file : {"module.ini", "scripts.txt", "troops.txt", "menus.txt"}) {
        const std::string p = game::vfs_join_path(module_dir, file);
        if (!vfs.is_file(p)) {
            missing.push_back(p);
        }
    }
    if (!vfs.is_file(game::vfs_join_path(module_dir, "Data/font_data.xml")) &&
        !vfs.is_file(root_path("Data/font_data.xml"))) {
        missing.push_back(root_path("Data/font_data.xml"));
    }
    if (!vfs.is_dir(root_path("CommonRes"))) {
        missing.push_back(root_path("CommonRes") + "/ (core resources)");
    }
    return missing;
}

// One readable message for `find_missing_warband_files`' result.
[[nodiscard]] inline std::string format_missing_warband_files(const std::vector<std::string>& missing) {
    std::string msg = "These Warband files were not found:\n";
    for (const std::string& m : missing) {
        msg += "  - " + m + "\n";
    }
    msg += "\nPick the Mount & Blade: Warband folder itself (the one that contains Modules, Data "
           "and CommonRes) in the launcher.";
    return msg;
}

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
    // Run `find_missing_warband_files` before loading (off for synthetic test modules that
    // lack a full install's files).
    bool check_install = true;
    // `rgba8`: draw with `SoftwareRhiDevice` into `pixels()` (capped at `max_render_height`).
    // `geometry`: record screen-space triangle batches at the full window size into
    // `geometry()` (`GeometryCaptureRhiDevice`) for the host to draw on the GPU; `pixels()`
    // stays empty and no CPU rasterizing happens.
    enum class Output : std::uint8_t { rgba8, geometry };
    Output output = Output::rgba8;
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
        missing_files_.clear();
        options_ = std::move(options);
        options_.run.drive_ui = true;

        // A wrong folder gets one message listing everything that is missing.
        if (const auto missing = options_.check_install
                                     ? find_missing_warband_files(vfs, options_.warband_root,
                                                                  options_.run.module_dir)
                                     : std::vector<std::string>{};
            !missing.empty()) {
            missing_files_ = missing;
            return fail(format_missing_warband_files(missing), error_out);
        }

        assets_ = std::make_unique<LoadedUiAssets>();
        std::string err;
        if (!load_native_ui_assets(vfs, options_.warband_root, options_.run.module_dir,
                                   options_.language, *assets_, &err)) {
            return fail("UI assets: " + err, error_out);
        }
        if (options_.output == UiGameShellOptions::Output::geometry) {
            auto geo = std::make_unique<GeometryCaptureRhiDevice>();
            geometry_ = geo.get();
            rhi_ = std::move(geo);
        } else {
            rhi_ = std::make_unique<SoftwareRhiDevice>();
        }
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
        geometry_ = nullptr;
        has_frame_ = false;
        assets_.reset();
        pixels_.clear();
        width_ = height_ = 0U;
        last_tree_hash_ = 0U;
        frames_reused_ = 0U;
        ++frame_serial_;  // a host holding the old frame re-checks it
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
    // `Output::geometry` only (else null): the last recorded frame, and the device that owns
    // its textures (`texture()` for pixel data, `take_destroyed_textures()` to free host copies).
    [[nodiscard]] const GeometryFrame* geometry() const noexcept {
        return geometry_ != nullptr && has_frame_ ? &geometry_->frame() : nullptr;
    }
    [[nodiscard]] GeometryCaptureRhiDevice* geometry_device() const noexcept { return geometry_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    // Bumped each time `pixels()` is redrawn; a host re-uploads its texture only on a change.
    [[nodiscard]] std::uint64_t frame_serial() const noexcept { return frame_serial_; }
    // Frames whose widget tree hashed the same as the last drawn one (rasterizer skipped).
    [[nodiscard]] std::uint64_t frames_reused() const noexcept { return frames_reused_; }
    [[nodiscard]] const std::vector<game::UiEvent>& last_events() const noexcept { return last_events_; }
    [[nodiscard]] bool running() const noexcept { return runner_ != nullptr && runner_->running(); }
    // Files the last failed `start` could not find (`find_missing_warband_files`); empty when
    // `start` succeeded or failed for another reason.
    [[nodiscard]] const std::vector<std::string>& missing_files() const noexcept {
        return missing_files_;
    }
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
        // The GPU path draws at native size; only the CPU rasterizer is capped.
        if (geometry_ == nullptr && options_.max_render_height != 0U &&
            h > options_.max_render_height) {
            w = std::max<std::uint32_t>(
                1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(w) *
                                               options_.max_render_height / h));
            h = options_.max_render_height;
        }
        UiRenderConfig cfg = options_.render;
        cfg.viewport_width = w;
        cfg.viewport_height = h;
        // Menus and most presentations sit still for many frames: keep the last frame when the
        // tree and the frame size are unchanged.
        const std::uint64_t tree_hash = UiTreeHasher::hash(ui_->root);
        if (has_frame_ && tree_hash == last_tree_hash_ && w == width_ && h == height_) {
            ++frames_reused_;
            return;
        }
        if (renderer_->render_tree(ui_->root, assets_->font, cfg, pixels_)) {
            width_ = w;
            height_ = h;
            last_tree_hash_ = tree_hash;
            has_frame_ = true;
            ++frame_serial_;
        }
    }

    UiGameShellOptions options_{};
    std::unique_ptr<LoadedUiAssets> assets_{};
    std::unique_ptr<RhiDevice> rhi_{};
    GeometryCaptureRhiDevice* geometry_ = nullptr;  // `rhi_` in `Output::geometry`
    bool has_frame_ = false;
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
    std::uint64_t last_tree_hash_ = 0U;
    std::uint64_t frame_serial_ = 0U;
    std::uint64_t frames_reused_ = 0U;
    std::vector<std::string> missing_files_{};
    bool in_game_ = false;
    bool quit_requested_ = false;
};

}  // namespace wse::platform
