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
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "wse/game/headless_runner.h"
#include "wse/platform/input.h"
#include "wse/platform/performance.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_geometry.h"
#include "wse/platform/rhi_ui.h"
#include "wse/platform/touch_overlay_render.h"
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

// Joins `rel` under `warband_root` (or keeps `rel` bare when `warband_root` is empty).
[[nodiscard]] inline std::string warband_root_path(std::string_view warband_root,
                                                   std::string_view rel) {
    return warband_root.empty() ? std::string(rel) : game::vfs_join_path(warband_root, rel);
}

// The files a module launch cannot do without, checked before anything loads so a wrong
// folder (the user picked `Modules/`, a module folder, or an unrelated directory instead of
// the Warband install) gets one message naming what is missing instead of the first loader
// error. Paths are relative to the VFS root; empty when the install looks complete. Not a full
// validation: a damaged file still fails later with the loader's own error.
[[nodiscard]] inline std::vector<std::string> find_missing_warband_files(
    Vfs& vfs, std::string_view warband_root, std::string_view module_dir) {
    std::vector<std::string> missing;
    const auto root_path = [&](std::string_view rel) {
        return warband_root_path(warband_root, rel);
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

// Escapes a UTF-8 string for a JSON string value (without surrounding quotes).
[[nodiscard]] inline std::string json_escape(std::string_view text) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(text.size());
    for (const unsigned char uc : text) {
        switch (uc) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (uc < 0x20U) {
                out.append("\\u00");
                out.push_back(kHex[(uc >> 4U) & 0x0fU]);
                out.push_back(kHex[uc & 0x0fU]);
            } else {
                out.push_back(static_cast<char>(uc));
            }
            break;
        }
    }
    return out;
}

// Serializes `find_missing_warband_files`' result as JSON for the Android launcher's
// pre-play install check (P10.3).
[[nodiscard]] inline std::string install_check_json(const std::vector<std::string>& missing) {
    std::string out = missing.empty() ? "{\"ok\":true,\"missing\":[" : "{\"ok\":false,\"missing\":[";
    for (std::size_t i = 0; i < missing.size(); ++i) {
        if (i != 0U) {
            out.push_back(',');
        }
        out.push_back('"');
        out.append(json_escape(missing[i]));
        out.push_back('"');
    }
    out.append("]}");
    return out;
}

// Lists the available Warband languages under `<warband_root>/languages/` for the launcher's
// language picker (P10.3): `"en"` first, then sorted valid language directories.
[[nodiscard]] inline std::vector<std::string> list_warband_languages(
    Vfs& vfs, std::string_view warband_root) {
    std::vector<std::string> languages{"en"};
    std::vector<VfsEntry> entries;
    if (!vfs.list_dir(warband_root_path(warband_root, "languages"), entries)) {
        return languages;
    }

    const auto is_valid_name = [](std::string_view name) noexcept {
        if (name.empty() || name.size() > 16U) {
            return false;
        }
        for (const char ch : name) {
            const bool allowed = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                 (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            if (!allowed) {
                return false;
            }
        }
        return true;
    };

    for (const VfsEntry& entry : entries) {
        if (!entry.is_directory || !is_valid_name(entry.name) ||
            ascii_lower(entry.name) == "en") {
            continue;
        }
        languages.push_back(entry.name);
    }

    std::sort(languages.begin() + 1, languages.end(),
              [](const std::string& a, const std::string& b) {
                  const std::string la = ascii_lower(a);
                  const std::string lb = ascii_lower(b);
                  if (la != lb) {
                      return la < lb;
                  }
                  return a < b;
              });
    return languages;
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

enum class UiShellStartState : std::uint8_t { idle, loading, ready, failed };

struct UiShellStartProgress {
    float fraction = 0.0f;
    std::string stage{};
};

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
    // Optional hooks invoked on the `start_async` load worker thread before VFS access and
    // before the thread exits (including on failure or cancellation); the Android host uses
    // these to attach/detach the worker to the JVM and rebind the SAF tree.
    std::function<void()> worker_thread_init{};
    std::function<void()> worker_thread_exit{};
    // Texture side cap (from `config/performance.json` `max_texture_size`, 0 = no cap).
    std::uint32_t max_texture_size = 0U;
    // UI canvas aspect ratio in the window (P11.1, from `config/performance.json` `ui_aspect`).
    UiAspectMode ui_aspect = UiAspectMode::Ratio4x3;
    // UI text scale multiplier (P11.2, from `config/performance.json` `ui_scale`).
    float ui_text_scale = 1.0f;

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
        auto work = std::make_unique<LoadWork>();
        work->vfs = &vfs;
        work->options = std::move(options);
        work->options.run.drive_ui = true;

        const bool ok = execute_load(*work);
        commit_work(*work);
        if (!ok) {
            if (error_out != nullptr) {
                *error_out = start_error_;
            }
            return false;
        }
        return true;
    }

    // Starts `start()`'s load work on a background thread and returns immediately. Returns false
    // if an async start is already in flight; otherwise tears down any previous run first.
    // `vfs` must outlive the shell (including any cancelled load until `poll_start` / `stop`).
    [[nodiscard]] bool start_async(Vfs& vfs, UiGameShellOptions options) {
        if (is_loading()) {
            return false;
        }
        stop();

        auto work = std::make_unique<LoadWork>();
        work->vfs = &vfs;
        work->options = std::move(options);
        work->options.run.drive_ui = true;

        pending_ = std::move(work);
        cancel_requested_.store(false, std::memory_order_release);
        worker_done_.store(false, std::memory_order_release);
        set_progress(0.05f, "Checking install");
        start_state_.store(UiShellStartState::loading, std::memory_order_release);

        worker_ = std::thread([this]() { run_worker(); });
        return true;
    }

    // Host-thread poll for `start_async()` (also reports `ready` / `failed` after synchronous
    // `start()`). Joins the worker and commits the loaded state once the worker has finished.
    [[nodiscard]] UiShellStartState poll_start() {
        const UiShellStartState cur = start_state_.load(std::memory_order_acquire);
        if (cur != UiShellStartState::loading) {
            return cur;
        }
        if (!worker_done_.load(std::memory_order_acquire)) {
            return UiShellStartState::loading;
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        if (pending_) {
            auto work = std::move(pending_);
            commit_work(*work);
        } else {
            start_error_ = "Async start finished with no result";
            start_state_.store(UiShellStartState::failed, std::memory_order_release);
        }
        worker_done_.store(false, std::memory_order_release);
        return start_state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] UiShellStartProgress start_progress() const {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        return progress_;
    }

    void cancel_start() noexcept {
        if (is_loading()) {
            cancel_requested_.store(true, std::memory_order_release);
        }
    }

    // Host thread: valid after `poll_start()` returns `failed` (or synchronous `start()` fails).
    [[nodiscard]] const std::string& start_error() const noexcept { return start_error_; }

    void stop() noexcept {
        cancel_requested_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        pending_.reset();
        worker_done_.store(false, std::memory_order_release);
        cancel_requested_.store(false, std::memory_order_release);
        reset_runtime();
        missing_files_.clear();
        start_error_.clear();
        {
            std::lock_guard<std::mutex> lock(progress_mutex_);
            progress_.fraction = 0.0f;
            progress_.stage.clear();
        }
        start_state_.store(UiShellStartState::idle, std::memory_order_release);
    }

    void feed(const RawInputEvent& ev) {
        if (is_loading()) {
            return;
        }
        input_.feed(ev);
    }
    void feed_text(std::string_view utf8) {
        if (is_loading()) {
            return;
        }
        input_.feed_text(utf8);
    }

    // Advances the module by `dt_s` (clamped to `[0, max_frame_seconds]`) with the input fed
    // since the last frame, computes the aspect-fitted viewport, maps the pointer through it,
    // and renders into that viewport. Returns whether the module is still running; the last
    // frame stays in `pixels()` / `geometry()` either way.
    bool frame(float dt_s, std::uint32_t window_w, std::uint32_t window_h) {
        if (is_loading() || runner_ == nullptr) {
            return false;
        }
        const float dt = std::isfinite(dt_s) ? std::clamp(dt_s, 0.0f, options_.max_frame_seconds)
                                             : 0.0f;
        const auto ticks =
            static_cast<std::int64_t>(std::llround(static_cast<double>(dt) * game::kTimerTicksPerSecond));
        const UiViewportRect vp = compute_ui_viewport(window_w, window_h, options_.ui_aspect);
        input_.set_viewport(vp, window_w, window_h);
        const game::UiPointerInput in = input_.take(static_cast<float>(ctx_->appTimeSeconds + dt));
        last_events_.clear();
        bool running = true;
        if (in_game_) {
            running = runner_->step(ticks, &in, &last_events_);
        } else {
            running = main_menu_frame(in);
        }
        render(window_w, window_h, vp);
        return running;
    }

    // Non-owning touch overlay configuration for `Output::geometry`: `profile` and
    // optional `input_state` must outlive `frame()`.
    void set_touch_overlay(const TouchOverlayProfile* profile,
                           InputContext context,
                           SafeAreaInsets safe_area = {},
                           const InputFrameState* input_state = nullptr) noexcept {
        touch_overlay_profile_ = profile;
        touch_overlay_context_ = context;
        touch_overlay_safe_area_ = safe_area;
        touch_overlay_state_ = input_state;
    }
    void set_input_context(InputContext context) noexcept {
        touch_overlay_context_ = context;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }
    // `Output::geometry` only (else null): the last recorded frame, and the device that owns
    // its textures (`texture()` for pixel data, `take_destroyed_textures()` to free host copies).
    [[nodiscard]] const GeometryFrame* geometry() const noexcept {
        return !is_loading() && geometry_ != nullptr && has_frame_ ? &geometry_frame_
                                                                    : nullptr;
    }
    [[nodiscard]] GeometryCaptureRhiDevice* geometry_device() const noexcept {
        return is_loading() ? nullptr : geometry_;
    }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    // Viewport rectangle in window pixels that the last frame rendered into.
    [[nodiscard]] UiViewportRect viewport() const noexcept {
        return is_loading() ? UiViewportRect{} : viewport_;
    }
    // Bumped each time `pixels()` is redrawn; a host re-uploads its texture only on a change.
    [[nodiscard]] std::uint64_t frame_serial() const noexcept { return frame_serial_; }
    // Frames whose widget tree hashed the same as the last drawn one (rasterizer skipped).
    [[nodiscard]] std::uint64_t frames_reused() const noexcept { return frames_reused_; }
    [[nodiscard]] const std::vector<game::UiEvent>& last_events() const noexcept { return last_events_; }
    [[nodiscard]] bool running() const noexcept {
        return !is_loading() && runner_ != nullptr && runner_->running();
    }
    // True when a text or number box overlay holds keyboard focus and the shell is running.
    [[nodiscard]] bool text_input_wanted() const noexcept {
        return running() && ctx_ != nullptr && ui_ != nullptr &&
               ui_->has_focused_text_input(ctx_->world);
    }
    // Files the last failed `start` could not find (`find_missing_warband_files`); empty when
    // `start` succeeded or failed for another reason.
    [[nodiscard]] const std::vector<std::string>& missing_files() const noexcept {
        return missing_files_;
    }
    // The main menu's quit button was pressed; the host should close.
    [[nodiscard]] bool quit_requested() const noexcept {
        return !is_loading() && quit_requested_;
    }
    // `game_start` has run and module frames are stepping.
    [[nodiscard]] bool in_game() const noexcept { return !is_loading() && in_game_; }
    [[nodiscard]] game::HeadlessRunReport report() const {
        return (!is_loading() && runner_ != nullptr) ? runner_->report()
                                                     : game::HeadlessRunReport{};
    }
    [[nodiscard]] game::UiSession* session() const noexcept {
        return is_loading() ? nullptr : ui_.get();
    }
    [[nodiscard]] vm::VmContext* vm() const noexcept { return is_loading() ? nullptr : ctx_.get(); }
    [[nodiscard]] std::size_t texture_bytes() const noexcept {
        return !is_loading() && assets_ != nullptr ? assets_->texture_bytes() : 0U;
    }

private:
    struct LoadWork {
        Vfs* vfs = nullptr;
        UiGameShellOptions options{};
        std::unique_ptr<LoadedUiAssets> assets{};
        std::unique_ptr<RhiDevice> rhi{};
        GeometryCaptureRhiDevice* geometry = nullptr;
        std::unique_ptr<UiRhiRenderer> renderer{};
        std::unique_ptr<vm::VmContext> ctx{};
        std::unique_ptr<game::ModuleBundle> bundle{};
        std::unique_ptr<game::UiSession> ui{};
        std::unique_ptr<game::HeadlessModuleRunner> runner{};
        bool in_game = false;
        std::vector<std::string> missing_files{};
        std::string error{};
        bool ok = false;

        void reset_built() noexcept {
            runner.reset();
            ui.reset();
            bundle.reset();
            ctx.reset();
            renderer.reset();
            rhi.reset();
            geometry = nullptr;
            assets.reset();
        }
    };

    [[nodiscard]] bool is_loading() const noexcept {
        return start_state_.load(std::memory_order_acquire) == UiShellStartState::loading;
    }

    void set_progress(float fraction, std::string_view stage) {
        std::lock_guard<std::mutex> lock(progress_mutex_);
        progress_.fraction = fraction;
        progress_.stage = std::string(stage);
    }

    void reset_runtime() noexcept {
        runner_.reset();  // before the session, VM and bundle it points into
        ui_.reset();
        bundle_.reset();
        ctx_.reset();
        renderer_.reset();  // before the device that owns its handles
        rhi_.reset();
        geometry_ = nullptr;
        geometry_frame_ = {};
        has_frame_ = false;
        assets_.reset();
        input_ = UiInputAccumulator{};
        last_events_.clear();
        pixels_.clear();
        width_ = height_ = 0U;
        viewport_ = UiViewportRect{};
        last_tree_hash_ = 0U;
        last_overlay_hash_ = 0U;
        last_text_scale_ = 1.0f;
        frames_reused_ = 0U;
        ++frame_serial_;  // a host holding the old frame re-checks it
        in_game_ = false;
        quit_requested_ = false;
    }

    bool execute_load(LoadWork& work) {
        const auto cancelled = [&]() noexcept {
            return cancel_requested_.load(std::memory_order_acquire);
        };
        const auto fail_load = [&](std::string message) -> bool {
            work.reset_built();
            if (cancelled()) {
                work.missing_files.clear();
                work.error = "Start cancelled";
            } else {
                work.error = std::move(message);
            }
            work.ok = false;
            return false;
        };

        set_progress(0.05f, "Checking install");
        // A wrong folder gets one message listing everything that is missing.
        if (work.options.check_install) {
            std::vector<std::string> missing = find_missing_warband_files(
                *work.vfs, work.options.warband_root, work.options.run.module_dir);
            if (!missing.empty()) {
                work.missing_files = std::move(missing);
                return fail_load(format_missing_warband_files(work.missing_files));
            }
        }
        if (cancelled()) {
            return fail_load("Start cancelled");
        }

        set_progress(0.20f, "Loading UI assets");
        work.assets = std::make_unique<LoadedUiAssets>();
        std::string err;
        if (!load_native_ui_assets(*work.vfs, work.options.warband_root,
                                   work.options.run.module_dir, work.options.language,
                                   *work.assets, &err, {}, work.options.max_texture_size)) {
            return fail_load("UI assets: " + err);
        }
        if (cancelled()) {
            return fail_load("Start cancelled");
        }

        set_progress(0.45f, "Preparing renderer");
        if (work.options.output == UiGameShellOptions::Output::geometry) {
            auto geo = std::make_unique<GeometryCaptureRhiDevice>();
            work.geometry = geo.get();
            work.rhi = std::move(geo);
        } else {
            work.rhi = std::make_unique<SoftwareRhiDevice>();
        }
        work.renderer = std::make_unique<UiRhiRenderer>(*work.rhi);
        if (!work.renderer->init(work.assets.get(), &err)) {
            return fail_load("UI renderer: " + err);
        }
        if (cancelled()) {
            return fail_load("Start cancelled");
        }

        set_progress(0.65f, "Loading module");
        work.ctx = std::make_unique<vm::VmContext>();
        work.bundle = std::make_unique<game::ModuleBundle>();
        work.ui = std::make_unique<game::UiSession>();
        work.ui->font = &work.assets->font;
        work.ui->lang = work.assets->lang;
        work.ui->vars = work.assets->vars;
        work.ui->mesh_bounds = collect_ui_mesh_bounds(*work.assets);
        game::HeadlessRunOptions run = work.options.run;
        if (work.options.start_at_initial_window) {
            run.run_game_start = false;
        }
        work.runner = std::make_unique<game::HeadlessModuleRunner>(
            *work.vfs, *work.ctx, *work.bundle, run, work.ui.get());
        if (!work.runner->start()) {
            const auto& rep = work.runner->report();
            return fail_load(rep.load_error.ok ? "game_start: " + rep.last_error
                                               : "module: " + rep.load_error.format());
        }
        if (cancelled()) {
            return fail_load("Start cancelled");
        }

        set_progress(0.90f, work.options.start_at_initial_window ? "Opening main menu"
                                                                 : "Starting module");
        work.in_game = !work.options.start_at_initial_window;
        if (!work.in_game) {
            work.ui->build_initial_window(work.ctx->world,
                                          module_display_name(work.options.run.module_dir),
                                          /*has_saves=*/false, /*can_new_game=*/true,
                                          /*can_tutorial=*/false, /*can_multiplayer=*/false);
        }
        if (cancelled()) {
            return fail_load("Start cancelled");
        }
        work.ok = true;
        return true;
    }

    void run_worker() {
        LoadWork& work = *pending_;
        {
            struct ExitGuard {
                const std::function<void()>& fn;
                ~ExitGuard() {
                    if (fn) {
                        fn();
                    }
                }
            };
            if (work.options.worker_thread_init) {
                work.options.worker_thread_init();
            }
            ExitGuard exit_guard{work.options.worker_thread_exit};
            (void)execute_load(work);
        }
        worker_done_.store(true, std::memory_order_release);
    }

    void commit_work(LoadWork& work) {
        if (!work.ok || cancel_requested_.load(std::memory_order_acquire)) {
            if (cancel_requested_.load(std::memory_order_acquire)) {
                work.reset_built();
                work.missing_files.clear();
                work.error = "Start cancelled";
            }
            reset_runtime();
            missing_files_ = std::move(work.missing_files);
            start_error_ = std::move(work.error);
            start_state_.store(UiShellStartState::failed, std::memory_order_release);
            return;
        }
        reset_runtime();
        options_ = std::move(work.options);
        assets_ = std::move(work.assets);
        rhi_ = std::move(work.rhi);
        geometry_ = work.geometry;
        renderer_ = std::move(work.renderer);
        ctx_ = std::move(work.ctx);
        bundle_ = std::move(work.bundle);
        ui_ = std::move(work.ui);
        runner_ = std::move(work.runner);
        in_game_ = work.in_game;
        missing_files_.clear();
        start_error_.clear();
        set_progress(1.0f, "Ready");
        start_state_.store(UiShellStartState::ready, std::memory_order_release);
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

    [[nodiscard]] std::uint64_t compute_overlay_hash() const noexcept {
        if (geometry_ == nullptr || touch_overlay_profile_ == nullptr) {
            return 0ULL;
        }
        std::uint64_t h = 0xcbf29ce484222325ULL;
        const auto mix = [&](const void* data, std::size_t n) noexcept {
            const auto* p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < n; ++i) {
                h = (h ^ p[i]) * 0x100000001b3ULL;
            }
        };
        const auto mix_pod = [&](const auto& v) noexcept { mix(&v, sizeof(v)); };
        mix_pod(touch_overlay_context_);
        mix_pod(touch_overlay_safe_area_.left);
        mix_pod(touch_overlay_safe_area_.top);
        mix_pod(touch_overlay_safe_area_.right);
        mix_pod(touch_overlay_safe_area_.bottom);
        mix_pod(touch_overlay_profile_->touch_overlay_enabled);
        for (const TouchWidgetLayout& w : touch_overlay_profile_->widgets) {
            mix_pod(w.cx);
            mix_pod(w.cy);
            mix_pod(w.half_w);
            mix_pod(w.half_h);
            mix_pod(w.scale);
            mix_pod(w.opacity);
            mix_pod(w.circular);
            mix_pod(w.enabled);
        }
        const bool has_state = (touch_overlay_state_ != nullptr);
        mix_pod(has_state);
        if (has_state) {
            mix_pod(touch_overlay_state_->touch_overlay_visible);
            mix_pod(touch_overlay_state_->stick_knob_x);
            mix_pod(touch_overlay_state_->stick_knob_y);
            mix_pod(touch_overlay_state_->attack_dir);
            mix_pod(touch_overlay_state_->attack_active);
            mix_pod(touch_overlay_state_->block_dir);
            mix_pod(touch_overlay_state_->block_active);
            mix_pod(touch_overlay_state_->radial_open);
            mix_pod(touch_overlay_state_->radial_level);
            mix_pod(touch_overlay_state_->radial_group);
            mix_pod(touch_overlay_state_->radial_sector);
            mix_pod(touch_overlay_state_->map_follow);
            mix_pod(touch_overlay_state_->map_pan_mode);
            mix_pod(touch_overlay_state_->map_paused);
            mix_pod(touch_overlay_state_->widget_pressed);
        }
        return (h == 0ULL) ? 1ULL : h;
    }

    void render(std::uint32_t window_w, std::uint32_t window_h, UiViewportRect vp) {
        if (window_w == 0U || window_h == 0U || vp.width == 0U || vp.height == 0U) {
            return;
        }
        std::uint32_t w = window_w;
        std::uint32_t h = window_h;
        if (geometry_ != nullptr) {
            geometry_->set_viewport(vp.x, vp.y, vp.width, vp.height);
        } else {
            w = vp.width;
            h = vp.height;
            // The GPU path draws at the full window size into `vp`; only the CPU rasterizer
            // scales the viewport down to `max_render_height`.
            if (options_.max_render_height != 0U && h > options_.max_render_height) {
                w = std::max<std::uint32_t>(
                    1U, static_cast<std::uint32_t>(static_cast<std::uint64_t>(w) *
                                                   options_.max_render_height / h));
                h = options_.max_render_height;
            }
        }
        UiRenderConfig cfg = options_.render;
        cfg.viewport_width = w;
        cfg.viewport_height = h;
        cfg.text_scale = options_.ui_text_scale;
        const float eff_text_scale = rhi_ui_detail::clamp_ui_text_scale(cfg.text_scale);
        // Menus and most presentations sit still for many frames: keep the last frame when the
        // tree, the overlay state, the frame size, the viewport and the text scale are
        // unchanged.
        const std::uint64_t tree_hash = UiTreeHasher::hash(ui_->root);
        const std::uint64_t overlay_hash = compute_overlay_hash();
        if (has_frame_ && tree_hash == last_tree_hash_ && overlay_hash == last_overlay_hash_ &&
            w == width_ && h == height_ && vp == viewport_ &&
            eff_text_scale == last_text_scale_) {
            ++frames_reused_;
            return;
        }
        if (renderer_->render_tree(ui_->root, assets_->font, cfg, pixels_)) {
            if (geometry_ != nullptr) {
                geometry_frame_ = geometry_->frame();
                if (touch_overlay_profile_ != nullptr &&
                    touch_overlay_profile_->touch_overlay_enabled &&
                    context_allows_touch_overlay(touch_overlay_context_)) {
                    TouchOverlayRenderParams overlay_params{};
                    overlay_params.viewport_width = w;
                    overlay_params.viewport_height = h;
                    overlay_params.context = touch_overlay_context_;
                    overlay_params.safe_area = touch_overlay_safe_area_;
                    overlay_params.frame_state = touch_overlay_state_;
                    overlay_params.gamepad_hidden =
                        (touch_overlay_state_ != nullptr &&
                         !touch_overlay_state_->touch_overlay_visible);
                    (void)append_touch_overlay_geometry(
                        *touch_overlay_profile_, overlay_params, geometry_frame_);
                }
            }
            width_ = w;
            height_ = h;
            viewport_ = vp;
            last_tree_hash_ = tree_hash;
            last_overlay_hash_ = overlay_hash;
            last_text_scale_ = eff_text_scale;
            has_frame_ = true;
            ++frame_serial_;
        }
    }

    UiGameShellOptions options_{};
    std::unique_ptr<LoadedUiAssets> assets_{};
    std::unique_ptr<RhiDevice> rhi_{};
    GeometryCaptureRhiDevice* geometry_ = nullptr;  // `rhi_` in `Output::geometry`
    GeometryFrame geometry_frame_{};
    bool has_frame_ = false;
    std::unique_ptr<UiRhiRenderer> renderer_{};
    std::unique_ptr<vm::VmContext> ctx_{};
    std::unique_ptr<game::ModuleBundle> bundle_{};
    std::unique_ptr<game::UiSession> ui_{};
    std::unique_ptr<game::HeadlessModuleRunner> runner_{};
    UiInputAccumulator input_{};
    const TouchOverlayProfile* touch_overlay_profile_ = nullptr;
    InputContext touch_overlay_context_ = InputContext::Mission;
    SafeAreaInsets touch_overlay_safe_area_{};
    const InputFrameState* touch_overlay_state_ = nullptr;
    std::vector<game::UiEvent> last_events_{};
    std::vector<std::uint8_t> pixels_{};
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    UiViewportRect viewport_{};
    std::uint64_t last_tree_hash_ = 0U;
    std::uint64_t last_overlay_hash_ = 0U;
    float last_text_scale_ = 1.0f;
    std::uint64_t frame_serial_ = 0U;
    std::uint64_t frames_reused_ = 0U;
    std::vector<std::string> missing_files_{};
    std::string start_error_{};
    bool in_game_ = false;
    bool quit_requested_ = false;
    std::atomic<UiShellStartState> start_state_{UiShellStartState::idle};
    std::atomic<bool> worker_done_{false};
    std::atomic<bool> cancel_requested_{false};
    mutable std::mutex progress_mutex_{};
    UiShellStartProgress progress_{};
    std::unique_ptr<LoadWork> pending_{};
    std::thread worker_{};
};

}  // namespace wse::platform
