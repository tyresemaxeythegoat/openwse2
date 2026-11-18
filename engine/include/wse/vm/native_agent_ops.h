// Native agent & horse spawning opcode handlers (Milestone 5, 50 opcodes).
// Grounded in official WSE2 v1.1.5.5 PDB/binary decompilation:
//   agents (50): 1689, 1692, 1693, 1713, 1717, 1725, 1733, 1734, 1739, 1742..1746,
//                1748, 1749, 1752..1755, 1757..1764, 1767..1769, 1777, 1778, 1803,
//                1806..1809, 1825..1827, 1973, 2076, 2077, 2080..2083, 2670, 2671

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_mp_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

// Mirrors mbMission::getAgentRelation: explicit per-agent overrides win over team relations.
[[nodiscard]] inline std::int32_t native_m5_agent_relation(
    std::int32_t a_id,
    std::int32_t b_id,
    const game::WseWorldState& world) noexcept {
    if (!world.is_valid_agent(a_id) || !world.is_valid_agent(b_id)) {
        return 0;
    }
    const auto& a = world.agents[static_cast<std::size_t>(a_id)];
    const auto& b = world.agents[static_cast<std::size_t>(b_id)];
    if (a.has_relations) {
        for (const auto& rel : a.relations) {
            if (rel.first == b_id) {
                return rel.second;
            }
        }
    }
    if (b.has_relations) {
        for (const auto& rel : b.relations) {
            if (rel.first == a_id) {
                return rel.second;
            }
        }
    }
    if (a.team_no == b.team_no) {
        return 1;
    }
    if (world.is_valid_team(a.team_no) && world.is_valid_team(b.team_no)) {
        return world.teams[static_cast<std::size_t>(a.team_no)]
            .relations[static_cast<std::size_t>(b.team_no)];
    }
    return -1;
}

// Mirrors mbActionGetMeleeAttackData: returns 1, 2, 3, or 0.
[[nodiscard]] inline std::int32_t native_m5_melee_attack_dir(
    std::int32_t action_no) noexcept {
    switch (action_no) {
    case 200:
    case 201:
    case 0xdc:
    case 0xdd:
    case 0xff:
    case 0x100:
    case 0x10e:
    case 0x10f:
    case 0x118:
    case 0x119:
    case 0x131:
    case 0x132:
        return 1;
    case 0xcd:
    case 0xce:
    case 0xe1:
    case 0xe2:
    case 0x104:
    case 0x105:
    case 0x113:
    case 0x114:
    case 0x11d:
    case 0x11e:
    case 0x12d:
    case 300:
        return 2;
    case 0xd2:
    case 0xd3:
    case 0xeb:
    case 0xec:
    case 0x109:
    case 0x10a:
    case 0x122:
    case 0x123:
    case 0x1de:
    case 0x1e4:
    case 0x1e5:
        return 3;
    default:
        return 0;
    }
}

// Mirrors mbActionGetMeleeDefendData for action_no - 0x136 < 0x48.
[[nodiscard]] inline std::int32_t native_m5_melee_defend_dir(
    std::int32_t action_no) noexcept {
    static constexpr std::array<std::uint8_t, 72> kDefendTable = {
        0, 1, 2, 15, 15, 15, 15, 15, 15, 0, 1, 2, 15, 15, 3, 4, 5, 15,
        15, 6, 7, 8, 15, 15, 9, 10, 11, 15, 15, 12, 13, 14, 15, 15, 3, 4,
        5, 15, 15, 6, 7, 8, 15, 15, 9, 10, 11, 15, 15, 12, 13, 14, 15, 15,
        3, 4, 5, 15, 15, 6, 7, 8, 15, 15, 9, 10, 11, 15, 15, 12, 13, 14};
    if (action_no < 0x136) {
        return -1;
    }
    const std::int32_t idx = action_no - 0x136;
    if (idx >= static_cast<std::int32_t>(kDefendTable.size())) {
        return -1;
    }
    const std::uint8_t entry = kDefendTable[static_cast<std::size_t>(idx)];
    if (entry <= 2U) {
        return 5;
    }
    if (entry <= 5U) {
        return 0;
    }
    if (entry <= 8U) {
        return 3;
    }
    if (entry <= 11U) {
        return 1;
    }
    if (entry <= 14U) {
        return 2;
    }
    return -1;
}

// Mirrors mbActionIsParried and mbActionIsBlocked.
[[nodiscard]] inline bool native_m5_is_parried_or_blocked_action(
    std::int32_t action_no) noexcept {
    switch (action_no) {
    case 0x80:
    case 0xdf:
    case 0xe0:
    case 0xe4:
    case 0xe5:
    case 0xe9:
    case 0xea:
    case 0xee:
    case 0xef:
    case 0xf3:
    case 0xf4:
    case 0xfd:
    case 0xfe:
    case 0x102:
    case 0x103:
    case 0x107:
    case 0x108:
    case 0x10c:
    case 0x10d:
    case 0x111:
    case 0x112:
    case 0x116:
    case 0x117:
    case 0x11b:
    case 0x11c:
    case 0x120:
    case 0x121:
    case 0x125:
    case 0x126:
    case 0x12a:
    case 299:
    case 0x12f:
    case 0x130:
    case 0x134:
    case 0x135:
    case 0x1dd:
    case 0x1e1:
    case 0x1e2:
    case 0x1e7:
    case 0x1e8:
    case 0x1ec:
    case 0x1ed:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline NativeDispatch dispatch_native_agent_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    auto& world = ctx.world;

    switch (opcode) {
    case 1689: // agent_get_speed (0x1401d3503)
        if (wse_register_ok(narrow[0]) && world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            const bool has_m5_speed = (ag.m5.speed.x != 0.0f || ag.m5.speed.y != 0.0f);
            ctx.positions[static_cast<std::size_t>(narrow[0])].o = wse::core::WseVec4{
                has_m5_speed ? ag.m5.speed.x : ag.speed.x,
                has_m5_speed ? ag.m5.speed.y : ag.speed.y,
                0.0f,
                0.0f};
        }
        return NativeDispatch::handled;
    case 1692: // agent_refill_wielded_shield_hit_points (0x1401d3597)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            const std::int32_t shield_item = ag.wielded_items[1];
            if (world.is_valid_item(shield_item) &&
                world.items[static_cast<std::size_t>(shield_item)].get_type() == 7) {
                const std::int32_t max_hp =
                    world.items[static_cast<std::size_t>(shield_item)].hit_points;
                for (std::size_t i = 0; i < ag.equipped_items.size(); ++i) {
                    if (ag.equipped_items[i] == shield_item) {
                        ag.m5.item_hit_points[i] = max_hp;
                    }
                }
            }
        }
        return NativeDispatch::handled;
    case 1693: // agent_is_in_special_mode (0x1401d35f1)
        if (world.is_valid_agent(narrow[0])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            return (ag.m5.scripted_mode != 0 || ag.has_scripted_destination)
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::condition_false;
    case 1713: // agent_set_look_target_agent (0x1401d3d4b)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_agent(narrow[1])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.is_non_player) {
                ag.m5.look_target_agent_no = narrow[1];
                ag.m5.look_target_flags = 2;
            }
        }
        return NativeDispatch::handled;
    case 1717: // agent_get_entry_no (0x1401d3e92)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].entry_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1725: // agent_set_invulnerable_shield (0x1401d42fc)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.invulnerable_shield =
                (wide[1] != 0);
        }
        return NativeDispatch::handled;
    case 1733: // agent_set_no_death_knock_down_only (0x1401d4783)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.no_death_knockdown_only =
                (wide[1] != 0);
        }
        return NativeDispatch::handled;
    case 1734: // agent_set_horse_speed_factor (0x1401d47ba)
        if (world.is_valid_agent(narrow[0]) && wide[1] >= 0) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.horse_speed_factor =
                static_cast<float>(wide[1]) / 100.0f;
        }
        return NativeDispatch::handled;
    case 1739: // agent_get_combat_state (0x1401d48ec)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].m5.combat_state),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1742: // agent_set_walk_forward_animation (0x1401d49e7)
        if (world.is_valid_agent(narrow[0]) &&
            (wide[1] == -1 ||
             (world.misc.num_actions > 0 ? world.is_valid_action(narrow[1]) : (narrow[1] >= 0)))) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.walk_action_no =
                (wide[1] == -1) ? -1 : narrow[1];
        }
        return NativeDispatch::handled;
    case 1743: // agent_set_animation_progress (0x1401d4a32)
        if (world.is_valid_agent(narrow[0]) && narrow[2] >= 0 && narrow[2] < 2) {
            const std::int64_t mod_scale = std::max<std::int64_t>(1, ctx.fixedPointMultiplier);
            const std::int64_t rem = wide[1] % mod_scale;
            world.agents[static_cast<std::size_t>(narrow[0])]
                .m5.action_channels[static_cast<std::size_t>(narrow[2])]
                .progress = static_cast<float>(rem) / static_cast<float>(mod_scale);
        }
        return NativeDispatch::handled;
    case 1744: // agent_set_look_target_position (0x1401d4aa5)
        if (world.is_valid_agent(narrow[0]) && wse_register_ok(narrow[1])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.is_non_player) {
                ag.m5.look_target_pos = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
                ag.m5.look_target_flags = 1;
                ag.look_position = ctx.positions[static_cast<std::size_t>(narrow[1])];
            }
        }
        return NativeDispatch::handled;
    case 1745: // agent_set_attack_action (0x1401d4b53)
        if (world.is_valid_agent(narrow[0]) && narrow[1] >= -2 && narrow[1] < 4 &&
            narrow[2] >= 0 && narrow[2] < 2) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            ag.m5.attack_override_type = narrow[1];
            ag.m5.attack_override_hold = narrow[2];
        }
        return NativeDispatch::handled;
    case 1746: // agent_set_defend_action (0x1401d4bbd)
        if (world.is_valid_agent(narrow[0]) && narrow[1] >= -2 && narrow[1] < 4 && wide[2] >= 0) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            ag.m5.defend_override_type = narrow[1];
            ag.m5.defend_override_duration = static_cast<float>(wide[2]) / 1000.0f;
        }
        return NativeDispatch::handled;
    case 1748: // agent_set_scripted_destination_no_attack (0x1401d4d11)
        if (world.is_valid_agent(narrow[0]) && wse_register_ok(narrow[1])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            // Copy: the script's position register must not be modified.
            wse::core::WseTransform pos =
                ctx.positions[static_cast<std::size_t>(narrow[1])];
            if (wide[2] != 0) {
                pos.o.z = world.ground_level_z;
            }
            ag.target_position = pos;
            ag.has_scripted_destination = true;
            ag.m5.scripted_mode = 4;
            if (statement.numOperands >= 4 && wide[3] == 0) {
                ag.m5.behavior_update_time = 0.0f;
            }
        }
        return NativeDispatch::handled;
    case 1749: // agent_fade_out (0x1401d4e44)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.m5.exit_status == 2) {
                ag.m5.running_away_state = 1;
                ag.m5.exit_status = 3;
                ag.m5.exit_timer_start = ctx.timersTotal[2];
                ag.running_away = true;
            }
        }
        return NativeDispatch::handled;
    case 1752: // agent_stop_running_away (0x1401d4f4c)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            ag.running_away = false;
            ag.m5.running_away = false;
            ag.m5.running_away_state = 0;
            ag.m5.behavior = 4;
        }
        return NativeDispatch::handled;
    case 1753: // agent_ai_set_aggressiveness (0x1401d4f79)
        if (world.is_valid_agent(narrow[0]) && wide[1] >= 0) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.is_non_player) {
                ag.m5.aggressiveness = static_cast<float>(wide[1]) / 1000.0f;
            }
        }
        return NativeDispatch::handled;
    case 1754: // agent_set_kick_allowed (0x1401d4fdd)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.can_kick = (wide[1] != 0);
        }
        return NativeDispatch::handled;
    case 1755: // remove_agent (0x1401d5014)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.is_alive()) {
                ag.m5.no_death_knockdown_only = false;
                ag.status = 0;
                ag.hit_points = 0.0f;
                ag.m5.death_timer_active = true;
                ag.m5.death_timer_start = ctx.timersTotal[2];
                if (world.is_valid_agent(ag.rider_agent_no)) {
                    world.agents[static_cast<std::size_t>(ag.rider_agent_no)].horse_agent_no = -1;
                }
                ag.rider_agent_no = -1;
                if (world.is_valid_agent(ag.horse_agent_no)) {
                    world.agents[static_cast<std::size_t>(ag.horse_agent_no)].rider_agent_no = -1;
                }
                ag.horse_agent_no = -1;
            }
        }
        return NativeDispatch::handled;
    case 1757: // agent_set_attached_scene_prop (0x1401d5167)
        if (world.is_valid_agent(narrow[0]) &&
            (wide[1] == -1 || world.is_valid_mission_object(narrow[1])) &&
            narrow[2] >= 0 && narrow[2] < game::kMaxAgentItems) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            const auto slot = static_cast<std::size_t>(narrow[2]);
            ag.attached_mission_object_nos[slot] = (wide[1] == -1) ? -1 : narrow[1];
            std::int32_t bone_no = 0;
            if (narrow[3] >= 0) {
                if (ag.m5.bones.empty()) {
                    bone_no = narrow[3];
                } else if (static_cast<std::size_t>(narrow[3]) < ag.m5.bones.size()) {
                    bone_no = narrow[3];
                }
            }
            ag.m5.attached_bone_nos[slot] = bone_no;
            ag.m5.attached_use_bone_rot[slot] =
                (statement.numOperands >= 5 && narrow[4] != 0);
        }
        return NativeDispatch::handled;
    case 1758: // agent_set_attached_scene_prop_x (0x1401d5227)
        if (world.is_valid_agent(narrow[0]) && narrow[2] >= 0 &&
            narrow[2] < game::kMaxAgentItems) {
            world.agents[static_cast<std::size_t>(narrow[0])]
                .m5.attached_offsets[static_cast<std::size_t>(narrow[2])]
                .x = static_cast<float>(wide[1]);
        }
        return NativeDispatch::handled;
    case 1759: // agent_set_attached_scene_prop_z (0x1401d527e)
        if (world.is_valid_agent(narrow[0]) && narrow[2] >= 0 &&
            narrow[2] < game::kMaxAgentItems) {
            world.agents[static_cast<std::size_t>(narrow[0])]
                .m5.attached_offsets[static_cast<std::size_t>(narrow[2])]
                .z = static_cast<float>(wide[1]);
        }
        return NativeDispatch::handled;
    case 1809: // agent_set_attached_scene_prop_y (0x1401d6c2f)
        if (world.is_valid_agent(narrow[0]) && narrow[2] >= 0 &&
            narrow[2] < game::kMaxAgentItems) {
            world.agents[static_cast<std::size_t>(narrow[0])]
                .m5.attached_offsets[static_cast<std::size_t>(narrow[2])]
                .y = static_cast<float>(wide[1]);
        }
        return NativeDispatch::handled;
    case 1760: // agent_get_time_elapsed_since_removed (0x1401d52d5)
        if (world.is_valid_agent(narrow[1])) {
            const float elapsed =
                world.agents[static_cast<std::size_t>(narrow[1])]
                    .m5.death_elapsed_seconds(ctx.timersTotal);
            store_lvalue(dst, wse_round64(elapsed), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1761: // agent_get_number_of_enemies_following (0x1401d5329)
        if (world.is_valid_agent(narrow[1])) {
            std::int32_t count = 0;
            for (std::size_t i = 0; i < world.agents.size(); ++i) {
                const auto idx = static_cast<std::int32_t>(i);
                const auto& other = world.agents[i];
                if (!other.active || !other.is_human()) {
                    continue;
                }
                if (native_m5_agent_relation(narrow[1], idx, world) == -1 &&
                    other.m5.ai_behavior_target_agent_no == narrow[1]) {
                    ++count;
                    if (world.is_valid_agent(other.horse_agent_no)) {
                        ++count;
                    }
                }
            }
            store_lvalue(dst, static_cast<std::int64_t>(count), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1762: // agent_set_no_dynamics (0x1401d5374)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.no_dynamics =
                (wide[1] != 0);
        }
        return NativeDispatch::handled;
    case 1763: // agent_get_attack_action (0x1401d53ab)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].m5.attack_action),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1764: // agent_get_defend_action (0x1401d53f2)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].m5.defend_action),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1767: // agent_get_action_dir (0x1401d54b1)
        if (world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            std::int32_t dir = -1;
            const std::int32_t ch1_action = ag.m5.action_channels[1].action_no;
            if (ag.m5.attack_action != 0) {
                dir = native_m5_melee_attack_dir(ch1_action);
            } else if (ag.m5.defend_action != 0) {
                dir = native_m5_melee_defend_dir(ch1_action);
            }
            store_lvalue(dst, static_cast<std::int64_t>(dir), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1768: // agent_get_animation (0x1401d556a)
        if (world.is_valid_agent(narrow[1]) && narrow[2] >= 0 && narrow[2] < 2) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.agents[static_cast<std::size_t>(narrow[1])]
                        .m5.action_channels[static_cast<std::size_t>(narrow[2])]
                        .action_no),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1769: // agent_is_in_parried_animation (0x1401d55d2)
        if (world.is_valid_agent(narrow[0])) {
            const std::int32_t ch1 =
                world.agents[static_cast<std::size_t>(narrow[0])]
                    .m5.action_channels[1]
                    .action_no;
            return native_m5_is_parried_or_blocked_action(ch1)
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::condition_false;
    case 1777: // agent_add_offer_with_timeout (0x1401d5933)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_agent(narrow[1])) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.add_offer(
                narrow[1], static_cast<float>(wide[2]) * 0.001f, ctx.timersTotal);
        }
        return NativeDispatch::handled;
    case 1778: // agent_check_offer_from_agent (0x1401d598d)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_agent(narrow[1])) {
            return world.agents[static_cast<std::size_t>(narrow[0])].m5.has_offer(
                       narrow[1], ctx.timersTotal)
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::condition_false;
    case 1803: // agent_add_relation_with_agent (0x1401d699e)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_agent(narrow[1]) &&
            narrow[2] >= -1 && narrow[2] < 2) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            bool updated = false;
            for (auto& rel : ag.relations) {
                if (rel.first == narrow[1]) {
                    rel.second = narrow[2];
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                ag.relations.emplace_back(narrow[1], narrow[2]);
            }
            ag.has_relations = true;
        }
        return NativeDispatch::handled;
    case 1806: // agent_is_alarmed (0x1401d6b43)
        if (world.is_valid_agent(narrow[0])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.is_non_player &&
                ((ag.m5.ai_behavior_flags & 1U) != 0U || ag.ai_alarmed)) {
                return NativeDispatch::handled;
            }
        }
        return NativeDispatch::condition_false;
    case 1807: // agent_set_is_alarmed (0x1401d6b8d)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.is_non_player) {
                if (wide[1] == 0) {
                    ag.m5.ai_behavior_flags &= ~1U;
                    ag.ai_alarmed = false;
                } else {
                    ag.m5.ai_behavior_flags |= 1U;
                    ag.ai_alarmed = true;
                }
            }
        }
        return NativeDispatch::handled;
    case 1808: // agent_stop_sound (0x1401d6be3)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (ag.m5.sound_channel >= 0) {
                if (world.audio_runtime != nullptr) {
                    world.audio_runtime->stop_channel(ag.m5.sound_channel);
                }
                ag.m5.sound_channel = -1;
            }
        }
        return NativeDispatch::handled;
    case 1825: // agent_get_ammo_for_slot (0x1401d70f4)
        if (world.is_valid_agent(narrow[1]) && narrow[2] >= 0 &&
            narrow[2] < game::kNumTroopEquipmentSlots) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            const auto slot = static_cast<std::size_t>(narrow[2]);
            const std::int32_t ammo =
                (ag.equipped_items[slot] >= 0) ? ag.item_ammo[slot] : 0;
            store_lvalue(dst, static_cast<std::int64_t>(ammo), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1826: // agent_is_in_line_of_sight (0x1401d7163)
        if (world.is_valid_agent(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            wse::core::WseVec4 origin = ag.position.o;
            origin.z += ag.m5.is_crouching ? 0.6f : 1.2f;
            wse::core::WseVec4 target = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            target.z += 0.1f;
            wse::core::WseVec4 delta{
                target.x - origin.x, target.y - origin.y, target.z - origin.z, 0.0f};
            const float dist = delta.length();
            if (dist <= 1e-6f) {
                return NativeDispatch::handled;
            }
            delta.normalize();
            const NativeRayHitResult hit = native_world_ray_aabb_hit(
                world, origin, delta, dist, 0xffffffffu);
            return hit.hit ? NativeDispatch::condition_false : NativeDispatch::handled;
        }
        return NativeDispatch::condition_false;
    case 1827: // agent_deliver_damage_to_agent_advanced (0x1401d72d5)
        // The weapon item is optional (-1 / omitted): explicit damage or the
        // deliverer's wielded weapon is used instead.
        if (world.is_valid_agent(narrow[1]) && world.is_valid_agent(narrow[2])) {
            const bool has_item =
                statement.numOperands >= 5 && world.is_valid_item(narrow[4]);
            auto& deliverer = world.agents[static_cast<std::size_t>(narrow[1])];
            auto& target = world.agents[static_cast<std::size_t>(narrow[2])];
            if (target.is_alive()) {
                std::int32_t base_damage = 0;
                if (narrow[3] > 0) {
                    base_damage = std::min<std::int32_t>(narrow[3], 1000);
                } else if (has_item) {
                    base_damage = native_campaign_item_weapon_damage(
                        world.items[static_cast<std::size_t>(narrow[4])]);
                } else if (world.is_valid_item(deliverer.wielded_items[0])) {
                    base_damage = native_campaign_item_weapon_damage(
                        world.items[static_cast<std::size_t>(deliverer.wielded_items[0])]);
                }
                const float raw_final = std::max(
                    0.0f, static_cast<float>(base_damage) * deliverer.damage_modifier);
                const std::int32_t rel = native_m5_agent_relation(narrow[1], narrow[2], world);
                std::int32_t inflicted = 0;
                if (!world.is_multiplayer || rel != 1) {
                    native_campaign_deliver_damage(
                        narrow[1], narrow[2], narrow[3], narrow[4], has_item, world);
                    inflicted = static_cast<std::int32_t>(raw_final);
                } else if (world.m5.server_friendly_fire == 1) {
                    const float ff_scale = std::clamp(
                        static_cast<float>(world.m5.server_ff_friend_ratio) / 100.0f, 0.0f, 500.0f);
                    const float ff_dmg = raw_final * ff_scale;
                    if (world.m5.server_ff_friend_ratio > 0) {
                        target.hit_points -= ff_dmg;
                        if (target.hit_points <= 0.0f) {
                            target.hit_points = 0.0f;
                            target.status = 3;
                        }
                        inflicted = static_cast<std::int32_t>(ff_dmg);
                    }
                    if (world.m5.server_ff_self_ratio > 0) {
                        const float self_scale = std::clamp(
                            static_cast<float>(world.m5.server_ff_self_ratio) / 100.0f, 0.0f, 500.0f);
                        deliverer.hit_points = std::max(0.0f, deliverer.hit_points - raw_final * self_scale);
                        if (deliverer.hit_points <= 0.0f) {
                            deliverer.status = 3;
                        }
                    }
                }
                deliverer.m5.last_inflicted_damage = inflicted;
                store_lvalue(dst, static_cast<std::int64_t>(inflicted), ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1973: // spawn_horse (0x1401da126)
        ctx.registers[0] = -1;
        // Safety deviation: capped at kMaxSpawnedAgents.
        if (world.is_valid_item(narrow[0])) {
            game::WseAgent horse{};
            horse.active = true;
            horse.status = 1;
            horse.type = 1;
            horse.horse_item_no = narrow[0];
            horse.position = world.spawn_position;
            horse.look_position = world.spawn_position;
            const std::int32_t item_hp =
                world.items[static_cast<std::size_t>(narrow[0])].hit_points;
            horse.max_hit_points = static_cast<float>(std::max<std::int32_t>(1, item_hp));
            horse.hit_points = horse.max_hit_points;
            ctx.registers[0] = static_cast<std::int64_t>(world.allocate_agent(std::move(horse)));
        }
        return NativeDispatch::handled;
    case 2076: // agent_get_bone_position (0x1401dc6eb)
        if (wse_register_ok(narrow[0]) && world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            if (narrow[2] >= 0 && static_cast<std::size_t>(narrow[2]) < ag.m5.bones.size()) {
                const wse::core::WseTransform& bone =
                    ag.m5.bones[static_cast<std::size_t>(narrow[2])].transform;
                ctx.positions[static_cast<std::size_t>(narrow[0])] =
                    (narrow[3] == 0) ? bone : ag.position.transform_to_parent(bone);
            }
        }
        return NativeDispatch::handled;
    case 2077: // agent_ai_set_interact_with_player (0x1401dc90b)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.ai_interact_with_player =
                (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 2080: // agent_ai_get_look_target (0x1401dc93e)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .m5.look_target_agent_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2081: // agent_ai_get_move_target (0x1401dc985)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .m5.ai_move_target_agent_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2082: // agent_ai_get_behavior_target (0x1401dc9cc)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .m5.ai_behavior_target_agent_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2083: // agent_ai_set_can_crouch (0x1401dca13)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].m5.ai_can_crouch =
                (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 2670: // agent_ai_get_num_cached_enemies (0x1401e0afb)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .m5.cached_enemies.size()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2671: // agent_ai_get_cached_enemy (0x1401e0b42)
        if (world.is_valid_agent(narrow[1])) {
            const auto& enemies =
                world.agents[static_cast<std::size_t>(narrow[1])].m5.cached_enemies;
            if (narrow[2] >= 0 && static_cast<std::size_t>(narrow[2]) < enemies.size()) {
                const std::int32_t enemy_id = enemies[static_cast<std::size_t>(narrow[2])];
                store_lvalue(dst,
                             static_cast<std::int64_t>(
                                 world.is_valid_agent(enemy_id) ? enemy_id : -1),
                             ctx, locals);
            }
        }
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_mp_statement(opcode, wide, narrow, tags, statement, ctx, locals);
}

}  // namespace wse::vm

#include "wse/vm/native_mp_ops.h"
