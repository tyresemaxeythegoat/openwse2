// Clean-room Phase 2H headless module execution runner.
//
// Port infrastructure built over clean-room module_bundle.h, trigger.h,
// block_vm.h, and the Vfs/LogSink seams so host CLI, unit tests, and arm64
// targets share one deterministic load -> game_start -> trigger-frame loop
// without Win32 or desktop filesystem dependencies inside engine/include.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/game/module_bundle.h"
#include "wse/game/trigger.h"
#include "wse/game/ui_runtime.h"
#include "wse/game/world_audio.h"
#include "wse/platform/audio.h"
#include "wse/platform/log.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"

namespace wse::game {

struct HeadlessRunOptions {
    std::string module_dir = "Modules/Native";
    ModuleLoadOptions load_options{};
    bool run_game_start = true;
    bool require_game_start = true;
    std::size_t trigger_frames = 1;
    std::int64_t frame_ticks = 200000;
    std::int32_t event_key = 0;
    bool fail_on_unimplemented = true;
    bool stop_on_error = true;
    bool enable_lua = true;
    std::string lua_storage_dir = "lua_storage";
    std::string lua_savegames_dir = "savegames";
    // Drive a `UiSession` after every frame: queued notes / presentation windows open,
    // presentations step (`-61`) and close when their duration runs out. Uses the
    // `UiSession*` passed to `run_headless_module` (`run_headless_session` passes its own),
    // or else one owned by the run.
    bool drive_ui = true;
    // A headless run models the campaign map: no mission scene is loaded, so
    // `world.lighting.has_scene` (which defaults to `true` for opcode tests) is cleared
    // after loading. Without this every frame counted as a mission frame, so
    // `start_presentation` never queued window 0x0f and `step_world_frame` never consumed the
    // campaign window queue. The port has no mission lifecycle yet to set it back.
    bool start_on_campaign_map = true;
    // Drive module audio: a `WorldAudioBridge` is attached before `game_start` (so sound and
    // music opcodes reach the mixer), and every frame moves the listener to the camera
    // (`listener_from_camera_transform`, `mbSoundManager::setTransform` `0x140333730`), runs
    // `WorldAudioBridge::update`, and pumps the mix into a `CaptureAudioDevice`.
    // `audio_codecs` decodes OGG / MP3 (`wse::asset::create_audio_codec_backend`, in the
    // `wse_audio` library); without it only WAV samples decode. `preload_audio` decodes every
    // sample at load instead of on first play (no decode hitch inside a play opcode).
    bool drive_audio = false;
    const asset::AudioCodecBackend* audio_codecs = nullptr;
    bool preload_audio = false;
    platform::LogSink* log = nullptr;
};

struct HeadlessRunReport {
    bool ok = false;
    ModuleLoadError load_error{};
    std::size_t loaded_files = 0;
    std::size_t skipped_files = 0;
    std::int32_t game_start_script_index = -1;
    bool game_start_executed = false;
    bool game_start_ok = false;
    std::size_t frames_executed = 0;
    bool triggers_ok = true;
    bool simple_triggers_ok = true;
    std::size_t error_count = 0;
    std::string last_error{};
    std::size_t unimplemented_count = 0;
    std::vector<std::pair<std::uint32_t, std::size_t>> unimplemented_opcodes{};
    int max_observed_depth = -1;
    std::size_t party_count = 0;
    std::size_t presentations_opened = 0;
    std::size_t notes_opened = 0;
    std::size_t menus_opened = 0;
    std::int32_t final_window = -1;  // `world.m5.active_window` after the last frame
    // `drive_audio` only: load-time path / decode stats and the stereo frames mixed.
    WorldAudioStats audio_stats{};
    std::size_t audio_frames_mixed = 0;
};

struct HeadlessSession {
    std::unique_ptr<vm::VmContext> ctx = std::make_unique<vm::VmContext>();
    std::unique_ptr<ModuleBundle> bundle = std::make_unique<ModuleBundle>();
    std::unique_ptr<UiSession> ui{};  // set when `HeadlessRunOptions::drive_ui`
    HeadlessRunReport report{};
};

namespace detail {

struct ScopedErrorForwarder {
    vm::VmContext& ctx;
    vm::VmErrorCallback prev_cb = nullptr;
    void* prev_user = nullptr;
    platform::LogSink* sink = nullptr;
    bool active = false;

    static void on_error(void* user, std::string_view message) {
        const auto* self = static_cast<const ScopedErrorForwarder*>(user);
        if (self->sink != nullptr) {
            self->sink->error(message);
        }
        if (self->prev_cb != nullptr) {
            self->prev_cb(self->prev_user, message);
        }
    }

    ScopedErrorForwarder(vm::VmContext& target, platform::LogSink* log_sink)
        : ctx(target),
          prev_cb(target.onError),
          prev_user(target.errorUser),
          sink(log_sink),
          active(log_sink != nullptr) {
        if (active) {
            ctx.onError = &on_error;
            ctx.errorUser = this;
        }
    }

    ~ScopedErrorForwarder() {
        if (active) {
            ctx.onError = prev_cb;
            ctx.errorUser = prev_user;
        }
    }

    ScopedErrorForwarder(const ScopedErrorForwarder&) = delete;
    ScopedErrorForwarder& operator=(const ScopedErrorForwarder&) = delete;
};

[[nodiscard]] inline std::int64_t saturating_add_ticks(
    std::int64_t base,
    std::int64_t delta) noexcept {
    if (delta > 0 && base > std::numeric_limits<std::int64_t>::max() - delta) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (delta < 0 && base < std::numeric_limits<std::int64_t>::min() - delta) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return base + delta;
}

inline void finalize_headless_report(const vm::VmContext& ctx,
                                     HeadlessRunReport& report,
                                     const HeadlessRunOptions& options) {
    report.error_count = ctx.errorCount;
    report.last_error = ctx.lastError;
    report.unimplemented_count = ctx.unimplementedCount;
    report.unimplemented_opcodes.clear();
    report.unimplemented_opcodes.reserve(ctx.unimplementedOpcodeCounts.size());
    for (const auto& [op, cnt] : ctx.unimplementedOpcodeCounts) {
        report.unimplemented_opcodes.emplace_back(op, cnt);
    }
    report.max_observed_depth = ctx.maxObservedDepth;
    report.party_count = ctx.world.parties.size();
    report.final_window = ctx.world.m5.active_window;

    const bool start_ok = !options.run_game_start ||
                          (report.game_start_script_index < 0
                               ? !options.require_game_start
                               : report.game_start_ok);
    report.ok = report.load_error.ok && start_ok &&
                report.triggers_ok && report.simple_triggers_ok &&
                report.error_count == 0U &&
                (!options.fail_on_unimplemented ||
                 report.unimplemented_count == 0U);
}

} // namespace detail

// One module run, stepped a frame at a time: `start` loads `options.module_dir` and runs
// `game_start`; each `step` advances the trigger timers, runs the trigger managers and the
// world frame, then (with `drive_ui`) the UI session. `run_headless_module` loops `step` for
// `trigger_frames`; an interactive host (`platform::UiGameShell`) calls it once per display
// frame with the elapsed time and that frame's pointer input. Not copyable or movable: the
// error forwarder and audio bridge point into it.
class HeadlessModuleRunner {
public:
    HeadlessModuleRunner(platform::Vfs& vfs,
                         vm::VmContext& ctx,
                         ModuleBundle& bundle,
                         HeadlessRunOptions options,
                         UiSession* ui = nullptr)
        : vfs_(vfs),
          ctx_(ctx),
          bundle_(bundle),
          options_(std::move(options)),
          err_scope_(ctx, options_.log) {
        // Without a caller-owned session, `drive_ui` uses one that lives with the runner.
        if (ui == nullptr && options_.drive_ui) {
            owned_ui_ = std::make_unique<UiSession>();
            ui = owned_ui_.get();
        }
        ui_ = ui;
    }

    HeadlessModuleRunner(const HeadlessModuleRunner&) = delete;
    HeadlessModuleRunner& operator=(const HeadlessModuleRunner&) = delete;

    // Loads the module, attaches audio and Lua, and runs `game_start`. Returns whether frames
    // may follow (false after a load failure, or a failed `game_start` with `stop_on_error`).
    [[nodiscard]] bool start() {
        ctx_.failOnUnimplemented = options_.fail_on_unimplemented;

        // Close any Lua state left by a previous run before the bundle it holds
        // callbacks into is reset; otherwise a failed reload or `enable_lua ==
        // false` would leave the old module's hooks active.
        if (ctx_.luaRuntime != nullptr) {
            ctx_.luaRuntime->shutdown();
        }

        report_.load_error = load_module(vfs_, options_.module_dir, ctx_, bundle_,
                                         options_.load_options);
        report_.loaded_files = bundle_.loaded_files.size();
        report_.skipped_files = bundle_.skipped_files.size();
        if (!report_.load_error.ok) {
            if (options_.log != nullptr) {
                options_.log->error(report_.load_error.format());
            }
            finalize();
            report_.ok = false;
            stopped_ = true;
            return false;
        }

        if (options_.start_on_campaign_map) {
            ctx_.world.lighting.has_scene = false;
        }

        if (options_.drive_audio) {
            report_.audio_stats =
                audio_.initialize(vfs_, ctx_.world, bundle_.sound_bank, bundle_.music_tracks,
                                  options_.module_dir, {}, options_.audio_codecs,
                                  options_.preload_audio);
            (void)audio_device_.open(platform::AudioDeviceSpec{});
        }

        if (options_.enable_lua && ctx_.luaRuntime != nullptr) {
            vm::WseLuaInitConfig lua_cfg{};
            lua_cfg.module_dir = options_.module_dir;
            lua_cfg.storage_dir = options_.lua_storage_dir;
            lua_cfg.savegames_dir = options_.lua_savegames_dir;
            lua_cfg.vfs = &vfs_;
            lua_cfg.bundle = &bundle_;
            lua_cfg.log = options_.log;
            if (ctx_.luaRuntime->initialize(ctx_, lua_cfg) && ctx_.luaRuntime->is_ready()) {
                ctx_.luaRuntime->on_game_load();
            }
        }

        report_.game_start_script_index = bundle_.scripts.mappedScripts[0];
        started_ = true;
        if (options_.run_game_start) {
            return run_game_start();
        }
        return true;
    }

    // Runs mapped script slot 0 (`game_start`, `mbGame::executeMappedScript` `0x1400dbfd0`).
    // `start` calls it when `options.run_game_start` is set; a host that shows a main menu
    // first (`platform::UiGameShell`) leaves that off and calls this on "Start a New Game".
    // Returns whether frames may follow.
    bool run_game_start() {
        if (!started_ || stopped_) {
            return false;
        }
        report_.game_start_script_index = bundle_.scripts.mappedScripts[0];
        if (report_.game_start_script_index >= 0) {
            report_.game_start_executed = true;
            report_.game_start_ok = vm::execute_mapped_script(bundle_.scripts, 0U, ctx_);
        } else {
            report_.game_start_executed = false;
            report_.game_start_ok = !options_.require_game_start;
            if (options_.require_game_start) {
                ctx_.report_error("Missing mapped script 'game_start'");
            }
        }
        if (options_.stop_on_error &&
            (!report_.game_start_ok || ctx_.errorCount > 0U || unimplemented_hit())) {
            finalize();
            stopped_ = true;
            return false;
        }
        return true;
    }

    // One frame of `frame_ticks` engine ticks (`kTimerTicksPerSecond` per second). `input`
    // (optional) is handed to `UiSession::handle_input` after the world frame and before
    // `drive_frame`, as `platform::run_ui_frame` orders them; its events are appended to
    // `ui_events`. Returns whether more frames may follow (false once `stop_on_error` trips).
    bool step(std::int64_t frame_ticks,
              const UiPointerInput* input = nullptr,
              std::vector<UiEvent>* ui_events = nullptr) {
        if (!started_ || stopped_) {
            return false;
        }
        const double frame_seconds = static_cast<double>(frame_ticks) / kTimerTicksPerSecond;
        for (auto& tick : ctx_.timersTotal) {
            tick = detail::saturating_add_ticks(tick, frame_ticks);
        }
        ctx_.appTimeSeconds += frame_seconds;

        const bool trig_ok = vm::execute_trigger_manager(
            bundle_.game_triggers, options_.event_key, ctx_.timersTotal, ctx_);
        const bool strig_ok = vm::execute_simple_trigger_manager(
            bundle_.simple_triggers, options_.event_key, ctx_.timersTotal, ctx_);
        if (options_.enable_lua && ctx_.luaRuntime != nullptr && ctx_.luaRuntime->is_ready()) {
            ctx_.luaRuntime->on_world_trigger(static_cast<float>(ctx_.appTimeSeconds / 3600.0));
        }
        const std::int32_t window_before = ctx_.world.m5.active_window;
        ctx_.world.step_world_frame(ctx_.timersTotal, static_cast<float>(frame_seconds));
        if (options_.drive_audio) {
            audio_.mixer().set_listener(
                platform::listener_from_camera_transform(ctx_.world.misc.camera_transform));
            audio_.update(ctx_.appTimeSeconds);
            // Mix one frame's worth of audio (the device is a capture sink, never drained).
            const auto frames = static_cast<std::size_t>(
                frame_seconds * static_cast<double>(audio_device_.spec().sample_rate));
            report_.audio_frames_mixed += audio_.mixer().pump_to_latency(
                audio_device_, audio_device_.queued_frames() + frames);
        }
        if (ui_ != nullptr && options_.drive_ui) {
            if (input != nullptr) {
                std::vector<UiEvent> ev =
                    ui_->handle_input(ctx_.world, *input, &ctx_, bundle_.presentations);
                if (ui_events != nullptr) {
                    ui_events->insert(ui_events->end(), ev.begin(), ev.end());
                }
            }
            const UiSession::FrameReport ui_rep =
                ui_->drive_frame(ctx_.world, ctx_, static_cast<float>(frame_seconds),
                                 bundle_.presentations, window_before, bundle_.menus);
            report_.presentations_opened += ui_rep.opened_presentation ? 1U : 0U;
            report_.notes_opened += ui_rep.opened_notes ? 1U : 0U;
            report_.menus_opened += ui_rep.opened_menu ? 1U : 0U;
        }
        report_.triggers_ok = report_.triggers_ok && trig_ok;
        report_.simple_triggers_ok = report_.simple_triggers_ok && strig_ok;
        ++report_.frames_executed;

        if (options_.stop_on_error &&
            (!trig_ok || !strig_ok || ctx_.errorCount > 0U || unimplemented_hit())) {
            stopped_ = true;
            return false;
        }
        return true;
    }

    // Refreshes the error / opcode / window fields and `ok` from the VM state.
    const HeadlessRunReport& finalize() {
        detail::finalize_headless_report(ctx_, report_, options_);
        return report_;
    }

    [[nodiscard]] const HeadlessRunReport& report() const noexcept { return report_; }
    [[nodiscard]] const HeadlessRunOptions& options() const noexcept { return options_; }
    [[nodiscard]] UiSession* ui() const noexcept { return ui_; }
    [[nodiscard]] bool running() const noexcept { return started_ && !stopped_; }

private:
    [[nodiscard]] bool unimplemented_hit() const noexcept {
        // Unimplemented opcodes only stop or fail the run in strict mode; with
        // `fail_on_unimplemented == false` they are counted and reported.
        return options_.fail_on_unimplemented && ctx_.unimplementedCount > 0U;
    }

    platform::Vfs& vfs_;
    vm::VmContext& ctx_;
    ModuleBundle& bundle_;
    HeadlessRunOptions options_;
    detail::ScopedErrorForwarder err_scope_;
    std::unique_ptr<UiSession> owned_ui_{};
    UiSession* ui_ = nullptr;
    WorldAudioBridge audio_{};
    platform::CaptureAudioDevice audio_device_{};
    HeadlessRunReport report_{};
    bool started_ = false;
    bool stopped_ = false;
};

// Loads `options.module_dir` via `vfs`, runs mapped script slot 0 (`game_start`)
// when requested, steps `options.trigger_frames` trigger ticks, and returns a
// diagnostic report enforcing zero VM errors and, unless
// `fail_on_unimplemented` is cleared, zero unimplemented opcodes. With `options.drive_ui`,
// every frame ends with `UiSession::drive_frame` on `ui` (or a run-local session when null).
[[nodiscard]] inline HeadlessRunReport run_headless_module(
    platform::Vfs& vfs,
    vm::VmContext& ctx,
    ModuleBundle& bundle,
    const HeadlessRunOptions& options = {},
    UiSession* ui = nullptr) {
    HeadlessModuleRunner runner(vfs, ctx, bundle, options, ui);
    if (!runner.start()) {
        return runner.report();
    }
    for (std::size_t frame = 0; frame < options.trigger_frames; ++frame) {
        if (!runner.step(options.frame_ticks)) {
            break;
        }
    }
    return runner.finalize();
}

// Convenience wrapper that heap-allocates VmContext and ModuleBundle so
// deep call-stacks and large Warband modules never stress caller thread stacks.
[[nodiscard]] inline HeadlessSession run_headless_session(
    platform::Vfs& vfs,
    const HeadlessRunOptions& options = {}) {
    HeadlessSession session{};
    if (options.drive_ui) {
        session.ui = std::make_unique<UiSession>();
    }
    session.report =
        run_headless_module(vfs, *session.ctx, *session.bundle, options, session.ui.get());
    return session;
}

} // namespace wse::game
