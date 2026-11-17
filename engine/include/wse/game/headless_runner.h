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
    // (`listener_from_camera_transform`, `mbSoundManager::setTransform` `0x140332970`), runs
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
    HeadlessRunReport report{};
    detail::ScopedErrorForwarder err_scope(ctx, options.log);
    // Without a caller-owned session, `drive_ui` uses one that lives for this run.
    std::unique_ptr<UiSession> owned_ui{};
    if (ui == nullptr && options.drive_ui) {
        owned_ui = std::make_unique<UiSession>();
        ui = owned_ui.get();
    }
    ctx.failOnUnimplemented = options.fail_on_unimplemented;
    // Unimplemented opcodes only stop or fail the run in strict mode; with
    // `fail_on_unimplemented == false` they are counted and reported.
    const auto unimplemented_hit = [&]() noexcept {
        return options.fail_on_unimplemented && ctx.unimplementedCount > 0U;
    };

    // Close any Lua state left by a previous run before the bundle it holds
    // callbacks into is reset; otherwise a failed reload or `enable_lua ==
    // false` would leave the old module's hooks active.
    if (ctx.luaRuntime != nullptr) {
        ctx.luaRuntime->shutdown();
    }

    report.load_error = load_module(vfs, options.module_dir, ctx, bundle,
                                    options.load_options);
    report.loaded_files = bundle.loaded_files.size();
    report.skipped_files = bundle.skipped_files.size();
    if (!report.load_error.ok) {
        if (options.log != nullptr) {
            options.log->error(report.load_error.format());
        }
        detail::finalize_headless_report(ctx, report, options);
        report.ok = false;
        return report;
    }

    if (options.start_on_campaign_map) {
        ctx.world.lighting.has_scene = false;
    }

    WorldAudioBridge audio{};
    platform::CaptureAudioDevice audio_device{};
    if (options.drive_audio) {
        report.audio_stats =
            audio.initialize(vfs, ctx.world, bundle.sound_bank, bundle.music_tracks,
                             options.module_dir, {}, options.audio_codecs, options.preload_audio);
        (void)audio_device.open(platform::AudioDeviceSpec{});
    }

    if (options.enable_lua && ctx.luaRuntime != nullptr) {
        vm::WseLuaInitConfig lua_cfg{};
        lua_cfg.module_dir = options.module_dir;
        lua_cfg.storage_dir = options.lua_storage_dir;
        lua_cfg.savegames_dir = options.lua_savegames_dir;
        lua_cfg.vfs = &vfs;
        lua_cfg.bundle = &bundle;
        lua_cfg.log = options.log;
        if (ctx.luaRuntime->initialize(ctx, lua_cfg) && ctx.luaRuntime->is_ready()) {
            ctx.luaRuntime->on_game_load();
        }
    }

    report.game_start_script_index = bundle.scripts.mappedScripts[0];
    if (options.run_game_start) {
        if (report.game_start_script_index >= 0) {
            report.game_start_executed = true;
            report.game_start_ok =
                vm::execute_mapped_script(bundle.scripts, 0U, ctx);
        } else {
            report.game_start_executed = false;
            report.game_start_ok = !options.require_game_start;
            if (options.require_game_start) {
                ctx.report_error("Missing mapped script 'game_start'");
            }
        }
        if (options.stop_on_error &&
            (!report.game_start_ok || ctx.errorCount > 0U ||
             unimplemented_hit())) {
            detail::finalize_headless_report(ctx, report, options);
            return report;
        }
    }

    const double frame_seconds =
        static_cast<double>(options.frame_ticks) / kTimerTicksPerSecond;
    for (std::size_t frame = 0; frame < options.trigger_frames; ++frame) {
        for (auto& tick : ctx.timersTotal) {
            tick = detail::saturating_add_ticks(tick, options.frame_ticks);
        }
        ctx.appTimeSeconds += frame_seconds;

        const bool trig_ok = vm::execute_trigger_manager(
            bundle.game_triggers, options.event_key, ctx.timersTotal, ctx);
        const bool strig_ok = vm::execute_simple_trigger_manager(
            bundle.simple_triggers, options.event_key, ctx.timersTotal, ctx);
        if (options.enable_lua && ctx.luaRuntime != nullptr &&
            ctx.luaRuntime->is_ready()) {
            ctx.luaRuntime->on_world_trigger(
                static_cast<float>(ctx.appTimeSeconds / 3600.0));
        }
        const std::int32_t window_before = ctx.world.m5.active_window;
        ctx.world.step_world_frame(ctx.timersTotal, static_cast<float>(frame_seconds));
        if (options.drive_audio) {
            audio.mixer().set_listener(
                platform::listener_from_camera_transform(ctx.world.misc.camera_transform));
            audio.update(ctx.appTimeSeconds);
            // Mix one frame's worth of audio (the device is a capture sink, never drained).
            const auto frames = static_cast<std::size_t>(
                frame_seconds * static_cast<double>(audio_device.spec().sample_rate));
            report.audio_frames_mixed +=
                audio.mixer().pump_to_latency(audio_device, audio_device.queued_frames() + frames);
        }
        if (ui != nullptr && options.drive_ui) {
            const UiSession::FrameReport ui_rep =
                ui->drive_frame(ctx.world, ctx, static_cast<float>(frame_seconds),
                                bundle.presentations, window_before, bundle.menus);
            report.presentations_opened += ui_rep.opened_presentation ? 1U : 0U;
            report.notes_opened += ui_rep.opened_notes ? 1U : 0U;
            report.menus_opened += ui_rep.opened_menu ? 1U : 0U;
        }
        report.triggers_ok = report.triggers_ok && trig_ok;
        report.simple_triggers_ok = report.simple_triggers_ok && strig_ok;
        ++report.frames_executed;

        if (options.stop_on_error &&
            (!trig_ok || !strig_ok || ctx.errorCount > 0U ||
             unimplemented_hit())) {
            break;
        }
    }

    detail::finalize_headless_report(ctx, report, options);
    return report;
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
