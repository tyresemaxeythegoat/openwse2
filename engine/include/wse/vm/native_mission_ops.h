// Native agent, team/order, scene prop, mission, menu, and timer opcode handlers
// (v1.1.5.5 mbStatement::execute).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "wse/asset/audio.h"
#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline bool native_agents_allied(const game::WseAgent& a,
                                               const game::WseAgent& b,
                                               const game::WseWorldState& world) noexcept {
    if (a.team_no == b.team_no) {
        return true;
    }
    if (world.is_valid_team(a.team_no) && world.is_valid_team(b.team_no)) {
        return world.teams[static_cast<std::size_t>(a.team_no)]
                   .relations[static_cast<std::size_t>(b.team_no)] > 0;
    }
    return false;
}

[[nodiscard]] inline bool native_agents_enemy(const game::WseAgent& a,
                                              const game::WseAgent& b,
                                              const game::WseWorldState& world) noexcept {
    if (a.team_no == b.team_no) {
        return false;
    }
    if (world.is_valid_team(a.team_no) && world.is_valid_team(b.team_no)) {
        return world.teams[static_cast<std::size_t>(a.team_no)]
                   .relations[static_cast<std::size_t>(b.team_no)] < 0;
    }
    return true;
}

[[nodiscard]] inline NativeDispatch dispatch_native_campaign_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

[[nodiscard]] inline NativeDispatch dispatch_native_mission_statement(
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
    // Agents
    case 1700: // get_player_agent_no (0x1401d36ee)
        store_lvalue(dst, static_cast<std::int64_t>(world.player_agent_no), ctx, locals);
        return NativeDispatch::handled;
    case 1702: // agent_is_alive (0x1401d3779)
        return (world.is_valid_agent(narrow[0]) &&
                world.agents[static_cast<std::size_t>(narrow[0])].is_alive())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1703: // agent_is_wounded (0x1401d37a6)
        return (world.is_valid_agent(narrow[0]) &&
                world.agents[static_cast<std::size_t>(narrow[0])].is_wounded_state())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1705: // get_player_agent_own_troop_kill_count (0x1401d3800)
        if (world.has_game) {
            const std::int32_t out =
                (wide[1] == 0) ? world.player_agent_own_troop_kill_count
                               : world.player_agent_own_troop_wound_count;
            store_lvalue(dst, static_cast<std::int64_t>(out), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1706: // agent_is_ally (0x1401d385d)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_agent(world.player_agent_no)) {
            return native_agents_allied(
                       world.agents[static_cast<std::size_t>(world.player_agent_no)],
                       world.agents[static_cast<std::size_t>(narrow[0])], world)
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::condition_false;
    case 1707: // agent_is_non_player (0x1401d38b6)
        return (world.is_valid_agent(narrow[0]) &&
                world.agents[static_cast<std::size_t>(narrow[0])].is_non_player &&
                narrow[0] != world.player_agent_no)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1708: // agent_is_defender (0x1401d38e3)
        if (!world.is_valid_agent(narrow[0])) {
            return NativeDispatch::condition_false;
        }
        if (world.is_valid_agent(world.player_agent_no)) {
            const auto& player_ag =
                world.agents[static_cast<std::size_t>(world.player_agent_no)];
            const auto& target_ag = world.agents[static_cast<std::size_t>(narrow[0])];
            const bool player_is_defender_side =
                (world.main_party_side == 0) ||
                (world.main_party_side == 2 && world.enemy_side == 1);
            const bool res = player_is_defender_side
                                 ? native_agents_allied(player_ag, target_ag, world)
                                 : native_agents_enemy(player_ag, target_ag, world);
            return res ? NativeDispatch::handled : NativeDispatch::condition_false;
        }
        return world.agents[static_cast<std::size_t>(narrow[0])].is_defender
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1710: // agent_get_position (0x1401d3a45)
        if (wse_register_ok(narrow[0]) && world.is_valid_agent(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.agents[static_cast<std::size_t>(narrow[1])].position;
        }
        return NativeDispatch::handled;
    case 1711: // agent_set_position (0x1401d3b2d)
        if (world.is_valid_agent(narrow[0]) && wse_register_ok(narrow[1])) {
            world.agents[static_cast<std::size_t>(narrow[0])].position =
                ctx.positions[static_cast<std::size_t>(narrow[1])];
        }
        return NativeDispatch::handled;
    case 1712: // agent_is_active (0x1401d3bb7)
        return world.is_valid_agent(narrow[0]) ? NativeDispatch::handled
                                               : NativeDispatch::condition_false;
    case 1714: // agent_get_horse (0x1401d3c3d)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].horse_agent_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1715: // agent_get_rider (0x1401d3c84)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].rider_agent_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1716: // agent_get_party_id (0x1401d3ccb)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].party_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1718: // agent_get_troop_id (0x1401d3d59)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].troop_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1720: // store_agent_hit_points (0x1401d3de7)
        if (world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            const float raw_hp =
                (wide[2] == 1)
                    ? ag.hit_points
                    : ((ag.hit_points * 100.0f) / std::max(1.0f, ag.max_hit_points));
            store_lvalue(dst, static_cast<std::int64_t>(raw_hp), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1721: // agent_set_hit_points (0x1401d3e5a)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            std::int32_t hp = narrow[1];
            if (wide[2] != 1) {
                hp = static_cast<std::int32_t>(
                    (static_cast<float>(wide[1]) * ag.max_hit_points) / 100.0f);
            }
            const std::int32_t hi =
                std::max<std::int32_t>(0, static_cast<std::int32_t>(ag.max_hit_points + 1.0f) - 1);
            ag.hit_points = static_cast<float>(std::clamp(hp, 0, hi));
        }
        return NativeDispatch::handled;
    case 1723: // agent_get_kill_count (0x1401d40c5)
        if (world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            store_lvalue(dst,
                         static_cast<std::int64_t>(wide[2] == 0 ? ag.kill_count
                                                                : ag.wound_count),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1726: // agent_get_wielded_item (0x1401d41b3)
        if (world.is_valid_agent(narrow[1]) && narrow[2] >= 0 && narrow[2] < 2) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .wielded_items[static_cast<std::size_t>(narrow[2])]),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1727: // agent_get_ammo (0x1401d4222)
        if (world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            std::int32_t ammo = 0;
            if (wide[2] == 0) {
                for (std::int32_t i = 0; i < game::kMaxAgentItems; ++i) {
                    if (ag.equipped_items[static_cast<std::size_t>(i)] >= 0) {
                        ammo += ag.item_ammo[static_cast<std::size_t>(i)];
                    }
                }
            } else {
                const std::int32_t wielded = ag.wielded_items[0];
                for (std::int32_t i = 0; i < game::kMaxAgentItems; ++i) {
                    if (ag.equipped_items[static_cast<std::size_t>(i)] == wielded &&
                        wielded >= 0) {
                        ammo = ag.item_ammo[static_cast<std::size_t>(i)];
                        break;
                    }
                }
            }
            store_lvalue(dst, static_cast<std::int64_t>(ammo), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1728: // agent_refill_ammo (0x1401d42a4)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            for (std::int32_t i = 0; i < game::kMaxAgentItems; ++i) {
                const std::int32_t item_no = ag.equipped_items[static_cast<std::size_t>(i)];
                if (world.is_valid_item(item_no)) {
                    ag.item_ammo[static_cast<std::size_t>(i)] =
                        world.items[static_cast<std::size_t>(item_no)].max_ammo;
                }
            }
        }
        return NativeDispatch::handled;
    case 1729: // agent_has_item_equipped (0x1401d42d1)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_item(narrow[1])) {
            const auto& items =
                world.agents[static_cast<std::size_t>(narrow[0])].equipped_items;
            for (const std::int32_t equipped : items) {
                if (equipped == narrow[1]) {
                    return NativeDispatch::handled;
                }
            }
        }
        return NativeDispatch::condition_false;
    case 1730: // agent_set_scripted_destination (0x1401d432e)
        if (world.is_valid_agent(narrow[0]) && wse_register_ok(narrow[1])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            auto& pos = ctx.positions[static_cast<std::size_t>(narrow[1])];
            if (wide[2] != 0) {
                pos.o.z = world.ground_level_z;
            }
            ag.target_position = pos;
            ag.has_scripted_destination = true;
            ag.m5.scripted_mode = 2;
        }
        return NativeDispatch::handled;
    case 1731: // agent_get_scripted_destination (0x1401d4463)
        if (wse_register_ok(narrow[0]) && world.is_valid_agent(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.agents[static_cast<std::size_t>(narrow[1])].target_position;
        }
        return NativeDispatch::handled;
    case 1732: // agent_force_rethink (0x1401d458c)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].ai_alarmed = true;
        }
        return NativeDispatch::handled;
    case 1735: // agent_clear_scripted_mode (0x1401d4688)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            ag.has_scripted_destination = false;
            ag.m5.scripted_mode = 0;
        }
        return NativeDispatch::handled;
    case 1736: // agent_set_speed_limit (0x1401d46b4)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].speed_limit =
                static_cast<float>(wide[1]) / 3.6f;
        }
        return NativeDispatch::handled;
    case 1738: // agent_get_simple_behavior (0x1401d4725)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .simple_behavior),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1740: // agent_set_animation (0x1401d47b3)
        if (world.is_valid_agent(narrow[0]) && (wide[1] == -1 || narrow[1] >= 0) &&
            narrow[2] >= 0 && narrow[2] < 2) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            ag.animation = narrow[1];
            ag.m5.action_channels[static_cast<std::size_t>(narrow[2])].action_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1741: // agent_set_stand_animation (0x1401d481c)
        if (world.is_valid_agent(narrow[0]) && (wide[1] == -1 || narrow[1] >= 0)) {
            world.agents[static_cast<std::size_t>(narrow[0])].stand_animation = narrow[1];
        }
        return NativeDispatch::handled;
    case 1747: // agent_set_wielded_item (0x1401d4abe)
        if (world.is_valid_agent(narrow[0]) &&
            (narrow[1] == -1 || world.is_valid_item(narrow[1]))) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            if (narrow[1] == -1) {
                ag.wielded_items = {-1, -1};
            } else {
                for (std::int32_t i = 0; i < game::kMaxAgentItems; ++i) {
                    if (ag.equipped_items[static_cast<std::size_t>(i)] == narrow[1]) {
                        ag.wielded_items[0] = narrow[1];
                        break;
                    }
                }
            }
        }
        return NativeDispatch::handled;
    case 1750: // agent_play_sound (0x1401d4d25)
        if (world.is_valid_agent(narrow[0]) && narrow[1] >= 0) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            world.played_sounds.push_back(game::WseBattleEvent{
                1, narrow[1], narrow[0], 0, ag.position.o});
            if (world.audio_runtime != nullptr) {
                // A looping sound the agent still owns would otherwise be orphaned
                // (agent_stop_sound only reaches the newest channel); one-shot
                // sounds are left to finish.
                if (ag.m5.sound_channel >= 0 &&
                    world.audio_runtime->is_channel_looping(ag.m5.sound_channel)) {
                    world.audio_runtime->stop_channel(ag.m5.sound_channel);
                }
                // Human NPC agents get the `mbAgent::playSound` (0x140057010) per-agent
                // voice pitch (`m_no`, `m_troopNo`, `m_partyNo`); player stays 1.0f.
                const float pitch =
                    (ag.type == 0 && narrow[0] != world.player_agent_no)
                        ? wse::asset::compute_agent_voice_pitch(narrow[0], ag.troop_no,
                                                                ag.party_no)
                        : 1.0f;
                const std::int32_t ch = world.audio_runtime->play_sound(
                    narrow[1], ag.position.o, 0U, pitch, 0);
                ag.m5.sound_channel = ch;
                world.music.last_sound_channel = ch;
            }
        }
        return NativeDispatch::handled;
    case 1756: // agent_get_attached_scene_prop (0x1401d4f83)
        if (world.is_valid_agent(narrow[1]) &&
            narrow[2] >= 0 && narrow[2] < game::kMaxAgentItems) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .attached_mission_object_nos[static_cast<std::size_t>(narrow[2])]),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1765: // agent_get_group (0x1401d52b9)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].group_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1766: // agent_set_group (0x1401d5300)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].group_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1770: // agent_get_team (0x1401d54a0)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].team_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1772: // agent_get_class (0x1401d5556)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].class_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1773: // agent_get_division (0x1401d55a1)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])].division_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1776: // agent_set_ammo (0x1401d5761)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_item(narrow[1]) &&
            narrow[2] >= 0) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            for (std::int32_t i = 0; i < game::kMaxAgentItems; ++i) {
                if (ag.equipped_items[static_cast<std::size_t>(i)] == narrow[1]) {
                    const std::int32_t cap =
                        world.items[static_cast<std::size_t>(narrow[1])].max_ammo;
                    ag.item_ammo[static_cast<std::size_t>(i)] =
                        cap > 0 ? std::clamp(narrow[2], 0, cap) : narrow[2];
                    break;
                }
            }
        }
        return NativeDispatch::handled;
    case 1783: // agent_set_division (0x1401d5deb)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].division_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1804: // agent_get_item_slot (0x1401d6880)
        if (world.is_valid_agent(narrow[1]) && narrow[2] >= 0 && narrow[2] < 8) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.agents[static_cast<std::size_t>(narrow[1])]
                                 .equipped_items[static_cast<std::size_t>(narrow[2])]),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    // Entry points, Orders, and Teams
    case 1780: // entry_point_get_position (0x1401d59f1)
        if (wse_register_ok(narrow[0]) &&
            narrow[1] >= 0 && narrow[1] < game::kMaxEntryPoints) {
            auto& ep = world.entry_points[static_cast<std::size_t>(narrow[1])];
            if (narrow[2] == 0 && ep.position.o.z < -100.0f) {
                ep.position.o.z = 0.0f;
            }
            ctx.positions[static_cast<std::size_t>(narrow[0])] = ep.position;
        }
        return NativeDispatch::handled;
    case 1789: // close_order_menu (0x1401d5f8a)
        world.order_menu_open = false;
        world.order_menu_type = -2;
        world.order_menu_subtype = -1;
        return NativeDispatch::handled;
    case 1790: // team_give_order (0x1401d5ff8)
        if (world.is_valid_team(narrow[0]) && narrow[2] >= 0 && narrow[2] < 27) {
            auto& tm = world.teams[static_cast<std::size_t>(narrow[0])];
            auto apply_div = [&](std::size_t d) noexcept {
                tm.order_types[d] = narrow[2];
                switch (narrow[2]) {
                case 3: tm.sub_orders.riding[d] = 1; break;
                case 4: tm.sub_orders.riding[d] = 2; break;
                case 7:
                    tm.sub_orders.gap_distance[d] = std::clamp(tm.sub_orders.gap_distance[d] - 1, 1, 6);
                    break;
                case 8:
                    tm.sub_orders.gap_distance[d] = std::clamp(tm.sub_orders.gap_distance[d] + 1, 1, 6);
                    break;
                case 9: tm.sub_orders.weapon_usage[d] = 1; break;
                case 10: tm.sub_orders.weapon_usage[d] = 0; break;
                case 15:
                    tm.sub_orders.weapon_usage[d] = 2;
                    tm.sub_orders.hold_fire[d] = 0;
                    break;
                case 16:
                    tm.sub_orders.weapon_usage[d] = 3;
                    tm.sub_orders.hold_fire[d] = 1;
                    break;
                default: break;
                }
            };
            if (narrow[1] >= 0 && narrow[1] < game::kNumDivisions) {
                apply_div(static_cast<std::size_t>(narrow[1]));
            } else {
                for (std::size_t d = 0; d < static_cast<std::size_t>(game::kNumDivisions); ++d) {
                    apply_div(d);
                }
            }
        }
        return NativeDispatch::handled;
    case 1791: // team_set_order_position (0x1401d6146)
        if (world.is_valid_team(narrow[0]) && wse_register_ok(narrow[2])) {
            auto& tm = world.teams[static_cast<std::size_t>(narrow[0])];
            const auto& pos = ctx.positions[static_cast<std::size_t>(narrow[2])];
            if (narrow[1] >= 0 && narrow[1] < game::kNumDivisions) {
                tm.order_positions[static_cast<std::size_t>(narrow[1])] = pos;
            } else {
                tm.order_positions.fill(pos);
            }
        }
        return NativeDispatch::handled;
    case 1792: // team_get_leader (0x1401d6284)
        if (world.is_valid_team(narrow[1])) {
            const auto& tm = world.teams[static_cast<std::size_t>(narrow[1])];
            std::int32_t leader = -1;
            for (const std::int32_t cand : tm.division_leaders) {
                if (world.is_valid_agent(cand)) {
                    leader = cand;
                    break;
                }
            }
            if (leader < 0 && world.is_valid_agent(tm.leader_agent_no)) {
                leader = tm.leader_agent_no;
            }
            store_lvalue(dst, static_cast<std::int64_t>(leader), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1793: // team_set_leader (0x1401d6301)
        if (world.is_valid_team(narrow[0]) && world.is_valid_agent(narrow[1])) {
            auto& tm = world.teams[static_cast<std::size_t>(narrow[0])];
            tm.leader_agent_no = narrow[1];
            tm.division_leaders.fill(narrow[1]);
        }
        return NativeDispatch::handled;
    case 1794: // team_get_order_position (0x1401d634a)
        if (world.is_valid_team(narrow[1]) && wse_register_ok(narrow[0]) &&
            narrow[2] >= 0 && narrow[2] < game::kNumDivisions) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].o =
                world.teams[static_cast<std::size_t>(narrow[1])]
                    .order_positions[static_cast<std::size_t>(narrow[2])].o;
        }
        return NativeDispatch::handled;
    case 1795: // team_set_order_listener (0x1401d6395)
        if (world.is_valid_team(narrow[0])) {
            world.teams[static_cast<std::size_t>(narrow[0])].order_listener = narrow[1];
        }
        return NativeDispatch::handled;
    case 1796: // team_set_relation (0x1401d648a)
        if (world.is_valid_team(narrow[0]) && world.is_valid_team(narrow[1]) &&
            narrow[2] >= -1 && narrow[2] < 2) {
            world.teams[static_cast<std::size_t>(narrow[0])]
                .relations[static_cast<std::size_t>(narrow[1])] = narrow[2];
            world.teams[static_cast<std::size_t>(narrow[1])]
                .relations[static_cast<std::size_t>(narrow[0])] = narrow[2];
        }
        return NativeDispatch::handled;

    // Scene Props / Mission Objects
    case 1810: { // scene_prop_get_num_instances (0x1401d6b06)
        std::int32_t count = 0;
        if (narrow[1] >= 0) {
            for (const auto& obj : world.mission_objects) {
                if (obj.active && obj.scene_prop_no == narrow[1]) {
                    ++count;
                }
            }
            store_lvalue(dst, static_cast<std::int64_t>(count), ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case 1811: { // scene_prop_get_instance (0x1401d6b49)
        if (narrow[1] >= 0 && narrow[2] >= 0) {
            std::int32_t seen = 0;
            for (std::size_t i = 0; i < world.mission_objects.size(); ++i) {
                const auto& obj = world.mission_objects[i];
                if (obj.active && obj.scene_prop_no == narrow[1]) {
                    if (seen == narrow[2]) {
                        store_lvalue(dst, static_cast<std::int64_t>(i), ctx, locals);
                        return NativeDispatch::handled;
                    }
                    ++seen;
                }
            }
            return NativeDispatch::condition_false;
        }
        return NativeDispatch::handled;
    }
    case 1812: // scene_prop_get_visibility (0x1401d6b9e)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                world.mission_objects[static_cast<std::size_t>(narrow[1])].visible ? 1 : 0,
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1813: // scene_prop_set_visibility (0x1401d6c07)
        if (world.is_valid_mission_object(narrow[0])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])].visible =
                (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1815: // scene_prop_get_hit_points (0x1401d6c99)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])].hit_points),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1817: // scene_prop_get_team (0x1401d6d2b)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])].team_no),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1818: // scene_prop_set_team (0x1401d6d72)
        if (world.is_valid_mission_object(narrow[0])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])].team_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1820: // scene_prop_set_cur_hit_points (0x1401d6df7)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            const auto hp = static_cast<float>(wide[1]);
            obj.hit_points = hp;
            if (hp > obj.max_hit_points) {
                obj.max_hit_points = hp;
            }
        }
        return NativeDispatch::handled;
    case 1890: // replace_scene_props (0x1401d8fc1)
        if (narrow[0] >= 0 && narrow[1] >= 0 &&
            static_cast<std::uint32_t>(ctx.currentCaller + 20) < 2U) {
            for (auto& obj : world.mission_objects) {
                if (obj.scene_prop_no == narrow[0]) {
                    obj.scene_prop_no = narrow[1];
                }
            }
        }
        return NativeDispatch::handled;

    // Mission, Menus, Triggers, Timers
    case 1906: // set_mission_result (0x1401d93c1)
        world.mission_result = narrow[0];
        return NativeDispatch::handled;
    case 1907: // finish_mission (0x1401d93d5)
        world.mission_finished = true;
        world.mission_finish_delay = static_cast<float>(narrow[0]);
        return NativeDispatch::handled;
    case 1911: // set_jump_mission (0x1401d940f)
        if (world.mission_templates.empty() ? (narrow[0] >= 0)
                                            : world.is_valid_mission_template(narrow[0])) {
            world.jump_mission_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 2060: // jump_to_menu (0x1401dc2b1)
        if (narrow[0] >= 0) {
            world.jump_menu_no = narrow[0];
            world.enqueue_window(game::kQueuedWindowMenuType);
        }
        return NativeDispatch::handled;
    case 2075: // set_trigger_result (0x1401dc55f)
        world.trigger_result = wide[0];
        return NativeDispatch::handled;
    case 2370: // store_mission_timer_a (0x1401df320)
        store_lvalue(dst, f2i_round(world.mission_timers[0].elapsed_seconds(ctx.timersTotal)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2371: // store_mission_timer_b (0x1401df358)
        store_lvalue(dst, f2i_round(world.mission_timers[1].elapsed_seconds(ctx.timersTotal)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2372: // store_mission_timer_c (0x1401df390)
        store_lvalue(dst, f2i_round(world.mission_timers[2].elapsed_seconds(ctx.timersTotal)),
                     ctx, locals);
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_campaign_statement(opcode, wide, narrow, tags, statement, ctx,
                                              locals);
}

}  // namespace wse::vm

#include "wse/vm/native_campaign_ops.h"
