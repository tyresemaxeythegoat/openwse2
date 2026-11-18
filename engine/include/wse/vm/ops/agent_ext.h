// Milestone 6 agent extension opcode handlers
// (59 WSE opcodes: 3300..3360 excluding 3340, 3341).

#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

#include "wse/core/math.h"
#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline bool wse_m6_valid_sound(
    const wse::game::WseWorldState& world,
    std::int32_t sound_no) noexcept {
    return sound_no >= 0 &&
           (world.m5.num_sounds <= 0 || sound_no < world.m5.num_sounds);
}

[[nodiscard]] inline const wse::game::WseM6AgentExt* wse_m6_find_agent(
    const wse::game::WseWorldState& world,
    std::int32_t agent_no) noexcept {
    const auto it = world.m6.agents.find(agent_no);
    return (it != world.m6.agents.end()) ? &it->second : nullptr;
}

[[nodiscard]] inline std::uint32_t wse_m6_agent_trigger_bit(
    std::int32_t trigger_id) noexcept {
    switch (trigger_id) {
    case -102:
        return 1U << 0U; // ti_on_agent_turn
    case -109:
        return 1U << 1U; // ti_on_agent_fill_collision_capsule
    case -110:
        return 1U << 2U; // ti_on_agent_fill_movement_capsule
    case -113:
        return 1U << 3U; // ti_on_agent_footstep_sound_played
    default:
        return 0U;
    }
}

[[nodiscard]] inline WseDispatch execute_wse_agent_ext(
    std::uint32_t opcode,
    const WseOperands& operands,
    WseEffect& effect) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;
    constexpr float kRadToDeg = 180.0f / kPi;

    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    auto* world = operands.state.world;
    const float fp_mul =
        static_cast<float>(operands.state.fixed_point_multiplier != 0
                               ? operands.state.fixed_point_multiplier
                               : 100);

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::agent_get_item_modifier: {
        // 3300: horse item modifier, or -1 if agent is not a horse.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& ag = world->agents[static_cast<std::size_t>(narrow[1])];
        if (ag.is_human()) {
            effect.value = -1;
        } else {
            const auto* agex = wse_m6_find_agent(*world, narrow[1]);
            effect.value = agex ? agex->horse_item_modifier : 0;
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_get_item_slot_modifier: {
        // 3301 (0x1401e8140)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            narrow[2] < 0 || narrow[2] >= 10) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        effect.value =
            agex ? agex->item_slot_modifiers[static_cast<std::size_t>(narrow[2])]
                 : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_get_animation_progress: {
        // 3302 (0x1401e819a)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t ch = (narrow.size() >= 3) ? narrow[2] : 0;
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            ch < 0 || ch >= 2) {
            return WseDispatch::ignored;
        }
        const auto& ag = world->agents[static_cast<std::size_t>(narrow[1])];
        effect.value = static_cast<std::int64_t>(static_cast<std::int32_t>(
            ag.m5.action_channels[static_cast<std::size_t>(ch)].progress *
            100.0f));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_get_dna: {
        // 3303 (0x1401e8200)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value = world->agents[static_cast<std::size_t>(narrow[1])].dna;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_get_ground_scene_prop: {
        // 3304 (0x1401e8234)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const std::int32_t mo_no =
            world->agents[static_cast<std::size_t>(narrow[1])]
                .m5.standing_mission_object_no;
        if (world->is_valid_mission_object(mo_no) &&
            !world->mission_objects[static_cast<std::size_t>(mo_no)].is_item) {
            effect.value = mo_no;
        } else {
            effect.value = -1;
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_item_slot_ammo: {
        // 3305 (0x1401e82c0)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 10) {
            return WseDispatch::ignored;
        }
        world->agents[static_cast<std::size_t>(narrow[0])]
            .item_ammo[static_cast<std::size_t>(narrow[1])] =
            std::max<std::int32_t>(0, narrow[2]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_item_slot_hit_points: {
        // 3306 (0x1401e8310)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            narrow[2] < 0 || narrow[2] >= 10) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->agents[static_cast<std::size_t>(narrow[1])]
                .m5.item_hit_points[static_cast<std::size_t>(narrow[2])];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_item_slot_hit_points: {
        // 3307 (0x1401e836a)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 10) {
            return WseDispatch::ignored;
        }
        auto& ag = world->agents[static_cast<std::size_t>(narrow[0])];
        const auto slot = static_cast<std::size_t>(narrow[1]);
        std::int32_t hp = std::max<std::int32_t>(0, narrow[2]);
        const std::int32_t item_no = ag.equipped_items[slot];
        if (world->is_valid_item(item_no)) {
            const std::int32_t max_hp =
                world->items[static_cast<std::size_t>(item_no)].hit_points;
            if (max_hp > 0) {
                hp = std::min(hp, max_hp);
            }
        }
        ag.m5.item_hit_points[slot] = hp;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_wielded_item_slot_no: {
        // 3308 (0x1401e83ba)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t hand = (narrow.size() >= 3) ? narrow[2] : 0;
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            hand < 0 || hand >= 2) {
            return WseDispatch::ignored;
        }
        // wielded_items holds item kinds; report the equipment slot carrying
        // the wielded kind, or -1 when the hand is empty.
        const auto& ag = world->agents[static_cast<std::size_t>(narrow[1])];
        const std::int32_t wielded =
            ag.wielded_items[static_cast<std::size_t>(hand)];
        std::int32_t slot_no = -1;
        if (wielded >= 0) {
            for (std::size_t i = 0; i < 4U; ++i) {
                if (ag.equipped_items[i] == wielded) {
                    slot_no = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
        effect.value = slot_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_get_scale: {
        // 3309 (0x1401e8401)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        const float scale = agex ? agex->scale : 1.0f;
        effect.value = static_cast<std::int32_t>(
            wse::core::wse_round64(fp_mul * scale));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_forced_lod: {
        // 3310 (0x1401e8449)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 6) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).forced_lod = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_item_slot_flags: {
        // 3311 (0x1401e849f)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            narrow[2] < 0 || narrow[2] >= 10) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        effect.value = agex
                           ? static_cast<std::int64_t>(
                                 agex->item_slot_flags[static_cast<std::size_t>(
                                     narrow[2])])
                           : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_ai_get_move_target_position: {
        // 3312 (0x1401e84e6)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >=
                operands.state.positions.size() ||
            world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        auto& pos =
            operands.state.positions[static_cast<std::size_t>(narrow[0])];
        pos.o = agex ? agex->ai_move_target_position
                     : wse::core::WseVec4{0.0f, 0.0f, 0.0f, 1.0f};
        pos.f = agex ? agex->ai_move_target_velocity
                     : wse::core::WseVec4{0.0f, 1.0f, 0.0f, 0.0f};
        pos.u = {0.0f, 0.0f, 1.0f, 0.0f};
        pos.orthonormalize();
        pos.rotate_z(kPi);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_horse: {
        // 3313 (0x1401e8619)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        // Validate the new horse before detaching the old one, so a rejected
        // call leaves both links intact.
        if (narrow[1] >= 0 && !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        auto& ag = world->agents[static_cast<std::size_t>(narrow[0])];
        if (world->is_valid_agent(ag.horse_agent_no)) {
            const std::int32_t old_horse_no = ag.horse_agent_no;
            auto& old_h = world->agents[static_cast<std::size_t>(old_horse_no)];
            old_h.rider_agent_no = -1;
            old_h.simple_behavior = 0;
            old_h.m5.behavior = 0;
            old_h.m5.behavior_update_time = 10.0f;
            world->m6.agent(old_horse_no).ai_behavior_update_time = 10.0f;
        }
        if (narrow[1] < 0) {
            ag.horse_agent_no = -1;
            return WseDispatch::handled_no_store;
        }
        world->agents[static_cast<std::size_t>(narrow[1])].rider_agent_no =
            narrow[0];
        ag.horse_agent_no = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_ai_set_simple_behavior: {
        // 3314 (0x1401e86ca)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 16) {
            return WseDispatch::ignored;
        }
        const std::int32_t gtime =
            (narrow.size() < 3 || narrow[2] <= 0) ? 10000000 : narrow[2];
        auto& ag = world->agents[static_cast<std::size_t>(narrow[0])];
        ag.simple_behavior = narrow[1];
        ag.m5.behavior = narrow[1];
        ag.m5.behavior_update_time = static_cast<float>(gtime);
        world->m6.agent(narrow[0]).ai_behavior_update_time =
            static_cast<float>(gtime);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_accelerate: {
        // 3315 (0x1401e8739)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            !wse_register_ok(narrow[1]) ||
            static_cast<std::size_t>(narrow[1]) >=
                operands.state.positions.size()) {
            return WseDispatch::ignored;
        }
        const auto& v =
            operands.state.positions[static_cast<std::size_t>(narrow[1])].o;
        auto& agex = world->m6.agent(narrow[0]);
        agex.acceleration.x += v.x;
        agex.acceleration.y += v.y;
        agex.acceleration.z += v.z;
        if (wide.size() >= 3 && wide[2] != 0) {
            agex.ghost_timer += static_cast<float>(wide[2]) / fp_mul;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_item_slot_modifier: {
        // 3316 (0x1401e87c1)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 10 || narrow[2] < 0 ||
            narrow[2] > 42) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0])
            .item_slot_modifiers[static_cast<std::size_t>(narrow[1])] =
            narrow[2];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_body_meta_mesh_set_vertex_keys_time_point: {
        // 3317 (0x1401e882f): body_meta_mesh inclusive 0..28, raw narrow[2].
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] > 28) {
            return WseDispatch::ignored;
        }
        auto& agex = world->m6.agent(narrow[0]);
        agex.body_mesh_deform_times[static_cast<std::size_t>(narrow[1])] =
            static_cast<float>(narrow[2]);
        agex.deform_vertex_keys_time = static_cast<float>(narrow[2]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_body_meta_mesh_set_visibility: {
        // 3318 (0x1401e888e)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] > 28) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).body_submesh_visibility[narrow[1]] =
            (narrow[2] != 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_personal_animation: {
        // 3319 (0x1401e88f2)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            !world->is_valid_action(narrow[1]) ||
            !world->is_valid_action(narrow[2])) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).personal_animations[narrow[1]] = narrow[2];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_personal_animation: {
        // 3320 (0x1401e8a66)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            !world->is_valid_action(narrow[2])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        std::int32_t res = narrow[2];
        if (agex != nullptr) {
            const auto it = agex->personal_animations.find(narrow[2]);
            if (it != agex->personal_animations.end()) {
                res = it->second;
            }
        }
        effect.value = res;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_default_animations: {
        // 3321 (0x1401e8abe)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).personal_animations.clear();
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_cancel_current_animation: {
        // 3322 (0x1401e8b55)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 2) {
            return WseDispatch::ignored;
        }
        auto& ag = world->agents[static_cast<std::size_t>(narrow[0])];
        ag.m5.action_channels[static_cast<std::size_t>(narrow[1])].action_no =
            -1;
        ag.m5.action_channels[static_cast<std::size_t>(narrow[1])].progress =
            0.0f;
        if (narrow[1] == 0) {
            ag.animation = -1;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_ranged_damage_modifier: {
        // 3323 (0x1401e8bb3)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& ag = world->agents[static_cast<std::size_t>(narrow[1])];
        effect.value = static_cast<std::int32_t>(
            wse::core::wse_round64(ag.ranged_damage_modifier * 100.0f));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_add_stun: {
        // 3324 (0x1401e8bf8)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 1) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).stun_time +=
            static_cast<float>(narrow[1]) / 1000.0f;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_body_meta_mesh_deform_in_range:
    case WseOpcode::agent_body_meta_mesh_deform_in_cycle_loop: {
        // 3325 / 3326 (0x1401e8c3d / 0x1401e8cbe)
        if (narrow.size() < 5 || wide.size() < 5) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] > 28) {
            return WseDispatch::ignored;
        }
        const auto mesh = static_cast<std::size_t>(narrow[1]);
        auto& agex = world->m6.agent(narrow[0]);
        agex.body_deform_start = narrow[2];
        agex.body_deform_end = narrow[3];
        agex.body_deform_duration = static_cast<float>(wide[4]) * 0.001f;
        agex.body_deform_loop =
            (opcode ==
             static_cast<std::uint32_t>(
                 WseOpcode::agent_body_meta_mesh_deform_in_cycle_loop));
        agex.body_mesh_deform_times[mesh] = static_cast<float>(wide[2]);
        const float prog = (agex.body_deform_duration > 0.0f) ? 0.0f : 1.0f;
        agex.body_deform_progress = prog;
        agex.body_mesh_deform_progress[mesh] = prog;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_body_meta_mesh_get_current_deform_progress: {
        // 3327 (0x1401e8d3f)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            narrow[2] < 0 || narrow[2] > 28) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        const float p =
            agex ? agex->body_mesh_deform_progress[static_cast<std::size_t>(
                       narrow[2])]
                 : 1.0f;
        effect.value = static_cast<std::int64_t>(
            static_cast<std::int32_t>(p * 100.0f));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_body_meta_mesh_get_current_deform_frame: {
        // 3328 (0x1401e8d94)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            narrow[2] < 0 || narrow[2] > 28) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        const float t =
            agex ? agex->body_mesh_deform_times[static_cast<std::size_t>(
                       narrow[2])]
                 : 0.0f;
        effect.value = wse::core::wse_round64(t);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_footstep_sound: {
        // 3329 (0x1401e8dee)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 5 ||
            (narrow[2] != -1 && !wse_m6_valid_sound(*world, narrow[2]))) {
            return WseDispatch::ignored;
        }
        auto& agex = world->m6.agent(narrow[0]);
        agex.footstep_sound_nos[static_cast<std::size_t>(narrow[1])] =
            narrow[2];
        agex.footstep_sound_no = narrow[2];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_horse_rotation_velocity: {
        // 3330 (0x1401e8e4a)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        effect.value = wse::core::wse_round64(
            fp_mul * (agex ? agex->horse_rotation_velocity : 0.0f));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_get_current_vertical_speed: {
        // 3331 (0x1401e8e87)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& ag = world->agents[static_cast<std::size_t>(narrow[1])];
        effect.value = static_cast<std::int32_t>(
            wse::core::wse_round64(ag.speed.z * 100.0f));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_current_vertical_speed: {
        // 3332 (0x1401e8ec1)
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        const float vy = static_cast<float>(wide[1]) / 100.0f;
        // speed.x/.y are the horizontal components read by agent_get_speed.
        world->agents[static_cast<std::size_t>(narrow[0])].speed.z = vy;
        world->m6.agent(narrow[0]).vertical_speed = vy;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_position_in_group: {
        // 3333 (0x1401e8f46)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >=
                operands.state.positions.size() ||
            world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        auto& pos =
            operands.state.positions[static_cast<std::size_t>(narrow[0])];
        pos.s = {1.0f, 0.0f, 0.0f, 0.0f};
        pos.f = {0.0f, 1.0f, 0.0f, 0.0f};
        pos.u = {0.0f, 0.0f, 1.0f, 0.0f};
        pos.o = agex ? agex->ai_formation_position
                     : world->agents[static_cast<std::size_t>(narrow[1])]
                           .position.o;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_current_ai_mesh_face_group: {
        // 3334 (0x1401e8fd9)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        effect.value = agex ? agex->current_ai_mesh_face_group : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_time_speed_multiplier: {
        // 3335 (0x1401e9039)
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).time_speed_multiplier =
            static_cast<float>(wide[1]) / fp_mul;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_time_speed_multiplier: {
        // 3336 (0x1401e907c)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        const float mult = agex ? agex->time_speed_multiplier : 1.0f;
        effect.value = static_cast<std::int32_t>(
            wse::core::wse_round64(fp_mul * mult));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_kick: {
        // 3337 (0x1401e90b9)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            (world->is_multiplayer && !world->is_server)) {
            return WseDispatch::ignored;
        }
        const auto& ag = world->agents[static_cast<std::size_t>(narrow[0])];
        if (!ag.is_non_player || !ag.m5.can_kick || !ag.is_human() ||
            ag.horse_agent_no >= 0) {
            return WseDispatch::handled_no_store;
        }
        world->m6.agent(narrow[0]).kick_requested = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_dropped_items_prune_time:
    case WseOpcode::agent_set_missile_items_prune_time: {
        // 3338 / 3339 (0x1401e91b1 / 0x1401e91f1): wide[1] seconds, NO fp_mul.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            wide[1] < 0) {
            return WseDispatch::ignored;
        }
        auto& agex = world->m6.agent(narrow[0]);
        if (opcode ==
            static_cast<std::uint32_t>(
                WseOpcode::agent_set_dropped_items_prune_time)) {
            agex.dropped_items_prune_time = static_cast<float>(wide[1]);
        } else {
            agex.missile_items_prune_time = static_cast<float>(wide[1]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_left_hand_weapon_collision:
    case WseOpcode::agent_ai_set_can_weapon_switch:
    case WseOpcode::agent_ai_set_can_fight:
    case WseOpcode::agent_set_enable_tilt:
    case WseOpcode::agent_ai_set_can_aim: {
        // 3342 / 3343 / 3344 / 3348 / 3352
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        auto& agex = world->m6.agent(narrow[0]);
        const bool val = (narrow[1] != 0);
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::agent_set_left_hand_weapon_collision:
            agex.left_hand_weapon_collision = val;
            break;
        case WseOpcode::agent_ai_set_can_weapon_switch:
            agex.ai_can_weapon_switch = val;
            break;
        case WseOpcode::agent_ai_set_can_fight:
            agex.ai_can_fight = val;
            break;
        case WseOpcode::agent_set_enable_tilt:
            agex.enable_tilt = val;
            break;
        default:
            agex.ai_can_aim = val;
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_fade_out_advanced:
    case WseOpcode::agent_fade_in_advanced: {
        // 3345 / 3346 (0x1401e9363 / 0x1401e93ea)
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        const float dur = static_cast<float>(wide[1]) / fp_mul;
        auto& ag = world->agents[static_cast<std::size_t>(narrow[0])];
        auto& agex = world->m6.agent(narrow[0]);
        if (opcode ==
            static_cast<std::uint32_t>(WseOpcode::agent_fade_out_advanced)) {
            agex.fade_out_duration = dur;
            agex.fade_out_alpha = 0.0f;
            ag.visible = false;
        } else {
            agex.fade_in_duration = dur;
            agex.fade_out_alpha = 1.0f;
            ag.visible = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_voice_sound: {
        // 3347 (0x1401e9471)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 8 ||
            (narrow[2] != -1 && !wse_m6_valid_sound(*world, narrow[2]))) {
            return WseDispatch::ignored;
        }
        auto& agex = world->m6.agent(narrow[0]);
        agex.voice_sound_nos[static_cast<std::size_t>(narrow[1])] = narrow[2];
        agex.voice_sound_no = narrow[2];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_rider_rotation_angles: {
        // 3349 (0x1401e94ff)
        if (narrow.size() < 4 || wide.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 12) {
            return WseDispatch::ignored;
        }
        auto& pair =
            world->m6.agent(narrow[0])
                .rider_rotation_angles[static_cast<std::size_t>(narrow[1])];
        pair[0] = (static_cast<float>(wide[2]) / fp_mul) * kDegToRad;
        pair[1] = (static_cast<float>(wide[3]) / fp_mul) * kDegToRad;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_rider_rotation_angles: {
        // 3350 (0x1401e9594)
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1]) ||
            narrow[2] < 0 || narrow[2] >= 12 || narrow[3] < 0 ||
            narrow[3] >= 2) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        const float rad =
            agex ? agex->rider_rotation_angles[static_cast<std::size_t>(
                       narrow[2])][static_cast<std::size_t>(narrow[3])]
                 : wse::game::kDefaultRiderRotationAngles
                       [static_cast<std::size_t>(narrow[2])]
                       [static_cast<std::size_t>(narrow[3])];
        effect.value = static_cast<std::int64_t>(
            static_cast<std::int32_t>((fp_mul * rad) * kRadToDeg));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_default_rider_rotation_angles: {
        // 3351 (0x1401e9608)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).rider_rotation_angles =
            wse::game::kDefaultRiderRotationAngles;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_set_can_multihit:
    case WseOpcode::agent_set_can_piercing: {
        // 3353 / 3359 (0x1401e9696 / 0x1401e970f)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::int32_t limit =
            (narrow.size() > 2) ? std::max<std::int32_t>(0, narrow[2]) : 0;
        auto& agex = world->m6.agent(narrow[0]);
        if (opcode ==
            static_cast<std::uint32_t>(WseOpcode::agent_set_can_multihit)) {
            agex.can_multihit = (narrow[1] != 0);
            agex.multihit_limit = limit;
        } else {
            agex.can_piercing = (narrow[1] != 0);
            agex.piercing_limit = limit;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_has_multihit:
    case WseOpcode::agent_has_piercing: {
        // 3354 / 3360 (0x1401e96e5 / 0x1401e975e)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[0]);
        const bool enabled =
            agex && (opcode == static_cast<std::uint32_t>(
                                   WseOpcode::agent_has_multihit)
                         ? agex->can_multihit
                         : agex->can_piercing);
        return enabled ? WseDispatch::handled_no_store
                       : WseDispatch::condition_false;
    }
    case WseOpcode::agent_set_crush_through: {
        // 3355 (0x1401e9788)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            narrow[1] < 0 || narrow[1] > 2) {
            return WseDispatch::ignored;
        }
        world->m6.agent(narrow[0]).crush_through_mode = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_crush_through: {
        // 3356 (0x1401e97cc)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto* agex = wse_m6_find_agent(*world, narrow[1]);
        effect.value = agex ? agex->crush_through_mode : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::agent_set_trigger_disabled: {
        // 3357 (0x1401e97f8 -> 0x14002b8b0)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint32_t bit = wse_m6_agent_trigger_bit(narrow[1]);
        if (world == nullptr || !world->is_valid_agent(narrow[0]) ||
            bit == 0U) {
            return WseDispatch::ignored;
        }
        auto& mask = world->m6.agent(narrow[0]).disabled_triggers;
        if (narrow[2] != 0) {
            mask |= bit;
        } else {
            mask &= ~bit;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_is_trigger_disabled: {
        // 3358 (0x1401e9832 -> 0x14002b8f0)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_agent(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint32_t bit = wse_m6_agent_trigger_bit(narrow[1]);
        const auto* agex = wse_m6_find_agent(*world, narrow[0]);
        const bool disabled =
            (bit != 0U && agex != nullptr &&
             (agex->disabled_triggers & bit) != 0U);
        return disabled ? WseDispatch::handled_no_store
                        : WseDispatch::condition_false;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
