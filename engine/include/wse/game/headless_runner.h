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
};

struct HeadlessSession {
    std::unique_ptr<vm::VmContext> ctx = std::make_unique<vm::VmContext>();
    std::unique_ptr<ModuleBundle> bundle = std::make_unique<ModuleBundle>();
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

    const bool start_ok = !options.run_game_start ||
                          (report.game_start_script_index < 0
                               ? !options.require_game_start
                               : report.game_start_ok);
    report.ok = report.load_error.ok && start_ok &&
                report.triggers_ok && report.simple_triggers_ok &&
                report.error_count == 0U && report.unimplemented_count == 0U;
}

} // namespace detail

// Loads `options.module_dir` via `vfs`, runs mapped script slot 0 (`game_start`)
// when requested, steps `options.trigger_frames` trigger ticks, and returns a
// diagnostic report enforcing zero VM errors / zero unimplemented opcodes.
[[nodiscard]] inline HeadlessRunReport run_headless_module(
    platform::Vfs& vfs,
    vm::VmContext& ctx,
    ModuleBundle& bundle,
    const HeadlessRunOptions& options = {}) {
    HeadlessRunReport report{};
    detail::ScopedErrorForwarder err_scope(ctx, options.log);
    ctx.failOnUnimplemented = options.fail_on_unimplemented;

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
             ctx.unimplementedCount > 0U)) {
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
        report.triggers_ok = report.triggers_ok && trig_ok;
        report.simple_triggers_ok = report.simple_triggers_ok && strig_ok;
        ++report.frames_executed;

        if (options.stop_on_error &&
            (!trig_ok || !strig_ok || ctx.errorCount > 0U ||
             ctx.unimplementedCount > 0U)) {
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
    session.report =
        run_headless_module(vfs, *session.ctx, *session.bundle, options);
    return session;
}

} // namespace wse::game
