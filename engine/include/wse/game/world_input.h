// Game-world input bridge: synchronizes `wse::platform::WseInputManager` into
// `wse::game::WseWorldState` (`world.input`, `world.m6.ui`, `world.m5.cursor_pos`)
// so Warband native input opcodes (`65`, `70..73`, `75`, `77`, `78`) and WSE
// extended input opcodes (`3100..3102`) observe live device & touch-overlay state
// (Slice P7, docs/PLATFORM_PLAN.md).
//
// Opcode VAs verified in `docs/SYMBOL_MAP.json` (`mbStatement::execute` and
// `mbStatement::executeWSE`):
//   - 65   game_key_get_mapped_key_name 0x1401c68d2
//   - 70   key_is_down                  0x1401c6965
//   - 71   key_clicked                  0x1401c69bf
//   - 72   game_key_is_down             0x1401c6a19
//   - 73   game_key_clicked             0x1401c6a71
//   - 75   mouse_get_position           0x1401c6ac9
//   - 77   omit_key_once                0x1401c6b35
//   - 78   clear_omitted_keys           0x1401c6b62
//   - 3100 game_key_get_key             0x1401e6bc2
//   - 3101 key_released                 0x1401e6c14
//   - 3102 game_key_released            0x1401e6c65

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "wse/game/world_state.h"
#include "wse/platform/input.h"

namespace wse::game {

class WorldInputBridge {
public:
    // Maps verified `kScreenWindow*` IDs (`world_subsystem_types.h`) onto `InputContext`.
    [[nodiscard]] static constexpr wse::platform::InputContext context_for_screen_window(
        std::int32_t active_window) noexcept {
        switch (active_window) {
        case kScreenWindowTactical:
            return wse::platform::InputContext::Mission;
        case kScreenWindowMap:
            return wse::platform::InputContext::CampaignMap;
        case kScreenWindowConversation:
            return wse::platform::InputContext::Dialog;
        case kScreenWindowPresentation:
            return wse::platform::InputContext::Presentation;
        case kScreenWindowMenu:
        case kScreenWindowNotes:
        case kScreenWindowLoading:
        default:
            return wse::platform::InputContext::Menu;
        }
    }

    static void sync_context_from_world(wse::platform::WseInputManager& mgr,
                                        const WseWorldState& world) noexcept {
        mgr.set_context(context_for_screen_window(world.m5.active_window));
    }

    // Copies the binding tables unconditionally (names for `65`, chords for `3100`).
    static void sync_bindings_to_world(const wse::platform::WseInputManager& mgr,
                                       WseWorldState& world) {
        const auto& km = mgr.game_keys();
        const std::size_t n =
            std::min(static_cast<std::size_t>(kMaxGameKeys),
                     static_cast<std::size_t>(wse::platform::kNumGameInputKeys));
        for (std::size_t i = 0; i < n; ++i) {
            const std::int32_t gk = static_cast<std::int32_t>(i);
            const auto* b = km.get(gk);
            if (b == nullptr) {
                continue;
            }
            world.input.game_key_names[i] = km.get_key_name(gk, 0);
            world.input.game_key_alt_names[i] = km.get_key_name(gk, 1);

            world.m6.ui.game_key_mappings[i][0] = {
                b->primary.key,
                b->primary.num_modifiers > 0 ? b->primary.modifiers[0] : -1,
                b->primary.num_modifiers > 1 ? b->primary.modifiers[1] : -1,
            };
            world.m6.ui.game_key_mappings[i][1] = {
                b->alt.key,
                b->alt.num_modifiers > 0 ? b->alt.modifiers[0] : -1,
                b->alt.num_modifiers > 1 ? b->alt.modifiers[1] : -1,
            };
        }
        world.input.bindings_revision = km.revision();
    }

    // Hands script-side `omit_key_once` (`77`) / `clear_omitted_keys` (`78`) state to the
    // manager. Call before `WseInputManager::step` / `process_frame` when not using
    // `step_and_sync`.
    static void pull_from_world(wse::platform::WseInputManager& mgr,
                                const WseWorldState& world) {
        mgr.set_omitted_keys(world.input.omitted_keys);
    }

    static void sync_to_world(const wse::platform::WseInputManager& mgr,
                              WseWorldState& world) {
        // `prev_*` are written so the VM's `!down && prev` tests in `3101` / `3102`
        // equal the manager's release edges, including a press + release inside
        // one frame.
        const std::size_t raw_n =
            std::min(world.input.key_down.size(), mgr.raw_down().size());
        for (std::size_t k = 0; k < raw_n; ++k) {
            const bool down = mgr.raw_down()[k];
            world.input.key_down[k] = down;
            world.input.key_clicked[k] = mgr.raw_clicked()[k];
            world.m6.ui.prev_key_down[k] = down ? mgr.raw_prev_down()[k] : mgr.raw_released()[k];
        }

        const std::size_t gk_n =
            std::min(world.input.game_key_down.size(), mgr.game_key_down().size());
        for (std::size_t g = 0; g < gk_n; ++g) {
            const bool down = mgr.game_key_down()[g];
            world.input.game_key_down[g] = down;
            world.input.game_key_clicked[g] = mgr.game_key_clicked()[g];
            world.m6.ui.prev_game_key_down[g] =
                down ? mgr.game_key_prev_down()[g] : mgr.game_key_released()[g];
        }

        // Consumed `omit_key_once` requests expire for scripts too.
        world.input.omitted_keys = mgr.omitted_keys();

        // Warband UI space (`mouse_get_position` `0x1401c6ac9`, `mouse_get_world_projection`
        // `0x1401cbe49`): x in [0, 1], y in [0, 0.75] growing upwards; the manager's cursor
        // is screen-normalized with y growing downwards.
        const auto& fs = mgr.frame_state();
        world.m5.cursor_pos = wse::core::WseVec2{fs.cursor_x, (1.0f - fs.cursor_y) * 0.75f};
        world.input.map_zoom_delta = fs.map_zoom_delta;
        if (fs.map_zoom_delta != 0.0f && std::isfinite(fs.map_zoom_delta) &&
            world.input.map_zoom_applied_frame != mgr.frame_counter()) {
            world.input.map_zoom_applied_frame = mgr.frame_counter();
            const float dist = (std::isfinite(world.input.map_camera_distance) &&
                                world.input.map_camera_distance > 0.0f)
                                   ? world.input.map_camera_distance
                                   : WseInputState::kDefaultMapCameraDistance;
            world.input.map_camera_distance =
                std::clamp(dist * std::exp(-fs.map_zoom_delta),
                           WseInputState::kMinMapCameraZoom,
                           WseInputState::kMaxMapCameraZoom);
        }

        if (world.input.bindings_revision != mgr.game_keys().revision()) {
            sync_bindings_to_world(mgr, world);
        }
    }

    // Processes one frame via `backend` and mirrors everything into `world`.
    static void step_and_sync(wse::platform::WseInputManager& mgr,
                              wse::platform::InputBackend& backend,
                              WseWorldState& world,
                              float dt_seconds = 1.0f / 60.0f) {
        pull_from_world(mgr, world);
        mgr.step(backend, dt_seconds);
        sync_to_world(mgr, world);
    }
};

}  // namespace wse::game
