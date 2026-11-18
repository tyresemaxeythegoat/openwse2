#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "wse/platform/clock.h"
#include "wse/platform/sleep.h"
#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline const Slots* wse_entity_slots_by_opcode(
    const wse::game::WseWorldState& world,
    std::uint32_t opcode,
    std::int32_t id) noexcept {
    if (id < 0) {
        return nullptr;
    }
    const auto idx = static_cast<std::size_t>(id);
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::item_slot_gt:
        return world.is_valid_item(id) ? &world.items[idx].slots : nullptr;
    case WseOpcode::party_template_slot_gt:
        return world.is_valid_party_template(id)
                   ? &world.party_templates[idx].slots
                   : nullptr;
    case WseOpcode::troop_slot_gt:
        return world.is_valid_troop(id) ? &world.troops[idx].slots : nullptr;
    case WseOpcode::faction_slot_gt:
        return world.is_valid_faction(id) ? &world.factions[idx].slots : nullptr;
    case WseOpcode::quest_slot_gt:
        return world.is_valid_quest(id) ? &world.quests[idx].slots : nullptr;
    case WseOpcode::scene_slot_gt:
        return world.is_valid_site(id) ? &world.sites[idx].slots : nullptr;
    case WseOpcode::party_slot_gt:
        return world.is_valid_party(id) ? &world.parties[idx].slots : nullptr;
    case WseOpcode::player_slot_gt:
        return world.is_valid_player(id) ? &world.players[idx].slots : nullptr;
    case WseOpcode::team_slot_gt:
        return world.is_valid_team(id) ? &world.teams[idx].slots : nullptr;
    case WseOpcode::agent_slot_gt:
        return world.is_valid_agent(id) ? &world.agents[idx].slots : nullptr;
    case WseOpcode::scene_prop_slot_gt:
        return world.is_valid_mission_object(id)
                   ? &world.mission_objects[idx].slots
                   : nullptr;
    default:
        return nullptr;
    }
}

[[nodiscard]] inline WseDispatch wse_entity_slot_gt(
    const WseOperands& operands,
    std::uint32_t opcode) noexcept {
    if (operands.narrow.size() < 3 || operands.wide.size() < 3) {
        return WseDispatch::operand_count_out_of_range;
    }
    if (operands.state.world == nullptr) {
        return WseDispatch::condition_false;
    }
    const Slots* slots = wse_entity_slots_by_opcode(
        *operands.state.world, opcode, operands.narrow[0]);
    if (slots == nullptr ||
        static_cast<std::uint32_t>(operands.narrow[1]) >
            static_cast<std::uint32_t>(wse::game::kMaxSlotIndex)) {
        return WseDispatch::condition_false;
    }
    return slots->get(operands.narrow[1]) > operands.wide[2]
               ? WseDispatch::handled_no_store
               : WseDispatch::condition_false;
}


[[nodiscard]] inline WseDispatch execute_wse_system(std::uint32_t opcode,
                                                     const WseOperands& operands,
                                                     WseEffect& effect) {
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    const auto registers = operands.registers;

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::register_get: {
        // 3000 (pseudocode line 628): checked index in the int64 register
        // bank, then the shared operand-0 store tail.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) || registers.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[1]);
        if (index >= registers.size()) {
            return WseDispatch::ignored;
        }
        effect.value = registers[index];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::register_set: {
        // 3001 (line 870): the destination is the register named by operand 0;
        // the value is operand 1. Retail returns true without returnValue.
        if (narrow.size() < 1 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || registers.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= registers.size()) {
            return WseDispatch::ignored;
        }
        registers[index] = wide[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::store_wse_version: {
        // 3002 (line 877): 0 yields the WSE version (5), 1 the Warband build
        // (0x483 = 1155); any other argument yields the zeroed r15 default.
        if (narrow.size() < 2 || wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t which = narrow[1];
        effect.value = which == 0 ? 5 : (which == 1 ? 0x483 : 0);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::item_slot_gt:
    case WseOpcode::party_template_slot_gt:
    case WseOpcode::troop_slot_gt:
    case WseOpcode::faction_slot_gt:
    case WseOpcode::quest_slot_gt:
    case WseOpcode::scene_slot_gt:
    case WseOpcode::party_slot_gt:
    case WseOpcode::player_slot_gt:
    case WseOpcode::team_slot_gt:
    case WseOpcode::agent_slot_gt:
    case WseOpcode::scene_prop_slot_gt:
        return wse_entity_slot_gt(operands, opcode);
    case WseOpcode::store_current_trigger: {
        // 3014 (0x1401e5a97): return the context flags while inside a trigger
        // (negative), otherwise 0. WSEOperationContext::GetCurrentTrigger uses
        // exactly this `m_context_flags < 0 ? m_context_flags : 0` rule.
        effect.value = operands.state.caller <= 0 ? operands.state.caller : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_num_return_values: {
        // 3016 (0x1401e5af9): g_numReturnValues at 0x140699b18.
        effect.value = operands.state.num_return_values;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_return_value: {
        // 3017 (0x1401e5b05): operand 1 is a 1-based index into the 16-slot
        // return-value bank at 0x140699b20. Out-of-range or unset indices log
        // ("invalid script result index %d" / "unset result value %d") and
        // return true without storing.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t index = narrow[1];
        if (index < 1 || index > 16) {
            return WseDispatch::ignored;
        }
        if (index > operands.state.num_return_values) {
            return WseDispatch::ignored;
        }
        const auto slot = static_cast<std::size_t>(index - 1);
        if (slot >= operands.state.return_values.size()) {
            return WseDispatch::ignored;
        }
        effect.value = operands.state.return_values[slot];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_application_time: {
        // 3035 (0x1401e6524): rglRound64((float)DXUtil_Timer(5) * 1000.0f).
        // FUN_1400b97f0(5) returns QPC seconds since the app-time baseline.
        effect.value = wse_round64(
            static_cast<float>(operands.state.app_time_seconds) * 1000.0f);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::sleep_ms: {
        // 3026 (0xbd2, 0x1401e6420 / L1188): negative skips Sleep(); invoke the
        // host sleep hook when provided so UI/render threads do not block.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (narrow[0] < 0) {
            return WseDispatch::ignored;
        }
        if (operands.state.sleep_ms_fn != nullptr) {
            operands.state.sleep_ms_fn(operands.state.sleep_ms_user, narrow[0]);
        } else {
            wse::platform::sleep_ms(narrow[0]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::set_main_party: {
        // 3030 (0xbd6, L1284): validates party via 0x1401c5050.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->main_party_no = narrow[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_main_party: {
        // 3031 (0xbd7, L1291): reads mbGame::m_mainPartyNo (g_game + 0x418).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game) {
            return WseDispatch::ignored;
        }
        effect.value = world->main_party_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::is_party_skill: {
        // 3036 (0xbdc, 0x1401e6544): skill > 41 logs and returns true; else
        // tests bit 4 of mbSkill::m_flags (0x14066091c).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr ||
            static_cast<std::uint32_t>(narrow[0]) >=
                static_cast<std::uint32_t>(wse::game::kMaxSkills)) {
            return WseDispatch::ignored;
        }
        const auto idx = static_cast<std::size_t>(narrow[0]);
        return (world->skills[idx].flags & wse::game::kSkillPartyFlagBit) != 0
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    // Options flags (0x10cc..0x10d1, 0x1401f0179..0x1401f01bb) and bool
    // config setters (3619/3622/4606).
    case WseOpcode::options_get_verbose_casualties:
    case WseOpcode::options_get_cheat_mode:
    case WseOpcode::options_get_realistic_headshots: {
        const bool* flag = wse_option_flag(operands.state.options, opcode);
        effect.value = (flag && *flag) ? 1 : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::options_set_verbose_casualties:
    case WseOpcode::options_set_cheat_mode:
    case WseOpcode::options_set_realistic_headshots:
    case WseOpcode::set_horse_friendly_fire:
    case WseOpcode::set_show_crosshair:
    case WseOpcode::edit_mode_set_enabled: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        bool* flag = wse_option_flag(operands.state.options, opcode);
        if (!flag) {
            return WseDispatch::ignored;
        }
        *flag = narrow[0] != 0;
        return WseDispatch::handled_no_store;
    }
    // VM return values / PRNG / time / timers / profiler (0xbc7..0xbe3).
    case WseOpcode::return_values: {
        // 3015 (0xbc7, 0x1401e5bd0): writes up to 16 int64 return slots.
        const std::size_t max_slots =
            std::min<std::size_t>(operands.state.return_values_out.size(), 16U);
        const std::size_t count = std::min<std::size_t>(wide.size(), max_slots);
        for (std::size_t i = 0; i < count; ++i) {
            operands.state.return_values_out[i] = wide[i];
        }
        if (operands.state.num_return_values_out != nullptr) {
            *operands.state.num_return_values_out = static_cast<std::int32_t>(
                std::min<std::size_t>(wide.size(), 16U));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::mtsrand: {
        // 3020 (0xbcc, 0x1401e5cf8): seeds rglMersenneTwister.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.rng == nullptr) {
            return WseDispatch::ignored;
        }
        operands.state.rng->seed(static_cast<std::uint32_t>(narrow[0]));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::mtrand: {
        // 3021 (0xbcd, 0x1401e5d05): rglMersenneTwister::getInt(min, max).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.rng == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = static_cast<std::int64_t>(
            operands.state.rng->get_int(narrow[1], narrow[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::get_time: {
        // 3022 (0xbce, 0x1401e5d22): UTC Unix epoch seconds (or local-adjusted
        // wall-clock seconds when operand 1 != 0), 32-bit int truncated.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        // Local time mirrors _localtime64_s + _mkgmtime64: the UTC value
        // plus the zone offset in effect at that instant.
        wse::platform::SystemWallClock system_clock;
        wse::platform::WallClock& wall =
            operands.state.wall_clock != nullptr ? *operands.state.wall_clock
                                                 : system_clock;
        std::int64_t seconds = static_cast<std::int64_t>(
            static_cast<std::int32_t>(wall.unix_seconds()));
        if (narrow.size() >= 2 && narrow[1] != 0) {
            seconds = static_cast<std::int64_t>(static_cast<std::int32_t>(
                seconds + wall.utc_offset_seconds(seconds)));
        }
        effect.value = seconds;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::timer_reset: {
        // 3027 (0xbd3, 0x1401e5f2b): op1 != 0 -> app clock (0), else game (2).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= operands.state.timers.size() ||
            operands.state.timers_total.size() < 3) {
            return WseDispatch::ignored;
        }
        const std::int32_t kind =
            (narrow.size() >= 2 && narrow[1] != 0) ? 0 : 2;
        operands.state.timers[static_cast<std::size_t>(narrow[0])] = {
            kind, operands.state.timers_total[static_cast<std::size_t>(kind)]};
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::timer_get_elapsed_time: {
        // 3028 (0xbd4, 0x1401e5f75): rounded elapsed seconds from 100kHz clock.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) ||
            static_cast<std::size_t>(narrow[1]) >= operands.state.timers.size()) {
            return WseDispatch::ignored;
        }
        const WseAppTimer& timer =
            operands.state.timers[static_cast<std::size_t>(narrow[1])];
        if (timer.kind < 0 ||
            static_cast<std::size_t>(timer.kind) >=
                operands.state.timers_total.size()) {
            return WseDispatch::ignored;
        }
        const std::int64_t total_now =
            operands.state.timers_total[static_cast<std::size_t>(timer.kind)];
        const float elapsed =
            static_cast<float>(total_now - timer.start_ticks) / 100000.0f;
        effect.value = wse_round64(elapsed);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::set_random_seed: {
        // 3034 (0xbda, 0x1401e643b): stores script rand_seed (DAT_140618d54).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.rand_seed == nullptr) {
            return WseDispatch::ignored;
        }
        *operands.state.rand_seed = narrow[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::profiler_start: {
        // 3040 (0xbe0, 0x140350590): sets g_profiling flag.
        if (operands.state.profiler_recording == nullptr) {
            return WseDispatch::ignored;
        }
        *operands.state.profiler_recording = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::profiler_stop: {
        // 3041 (0xbe1, 0x140350d00): clears g_profiling flag.
        if (operands.state.profiler_recording == nullptr) {
            return WseDispatch::ignored;
        }
        *operands.state.profiler_recording = false;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::profiler_is_recording: {
        // 3042 (0xbe2): returns g_profiling as boolean condition.
        const bool recording = operands.state.profiler_recording != nullptr &&
                               *operands.state.profiler_recording;
        return recording ? WseDispatch::handled_no_store
                         : WseDispatch::condition_false;
    }
    case WseOpcode::profiler_mark: {
        // 3043 (0xbe3, 0x140351660): appends marker when profiling is active.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string marker;
        if (!wse_resolve_utf8_operand(0, operands, marker)) {
            return WseDispatch::ignored;
        }
        if (operands.state.profiler_recording != nullptr &&
            *operands.state.profiler_recording &&
            operands.state.profiler_markers != nullptr) {
            operands.state.profiler_markers->push_back(std::move(marker));
        }
        return WseDispatch::handled_no_store;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
