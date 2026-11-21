#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include "wse/game/world_campaign_helpers.h"
#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_campaign_2_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

[[nodiscard]] inline std::int32_t native_campaign_achievement_key(
    std::int32_t achievement_id,
    std::int32_t stat_index) noexcept {
    return (achievement_id << 16) | (stat_index & 0xFFFF);
}

[[nodiscard]] inline bool native_campaign_all_enemies_defeated(
    const game::WseWorldState& world) noexcept {
    std::int32_t player_team = world.main_party_side;
    if (world.is_valid_agent(world.player_agent_no)) {
        player_team = world.agents[static_cast<std::size_t>(world.player_agent_no)].team_no;
    }
    for (const auto& ag : world.agents) {
        if (!ag.active || !ag.is_human() || !ag.is_alive() || ag.is_routed) {
            continue;
        }
        const bool rel_enemy =
            world.is_valid_team(ag.team_no) && world.is_valid_team(player_team) &&
            world.teams[static_cast<std::size_t>(ag.team_no)]
                    .relations[static_cast<std::size_t>(player_team)] < 0;
        const bool side_enemy =
            (ag.team_no == world.enemy_side && player_team != world.enemy_side);
        if (rel_enemy || side_enemy) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::int32_t native_campaign_num_active_teams(
    const game::WseWorldState& world) noexcept {
    std::array<bool, game::kNumTeams> has_alive{};
    for (const auto& ag : world.agents) {
        if (ag.active && ag.is_human() && ag.is_alive() && world.is_valid_team(ag.team_no)) {
            has_alive[static_cast<std::size_t>(ag.team_no)] = true;
        }
    }
    std::int32_t count = 0;
    for (const bool active : has_alive) {
        if (active) {
            ++count;
        }
    }
    return count;
}

// Mirrors mbGame::addExperienceAsReward (0x1400e6380): awards full xp to player_troop_no and
// divides xp evenly among non-prisoner companion heroes in main_party (stopping at first prisoner).
inline void native_campaign_add_xp_reward(std::int32_t xp_amount,
                                          game::WseWorldState& world) {
    if (xp_amount <= 0) {
        return;
    }
    if (world.is_valid_troop(world.player_troop_no)) {
        game::troop_add_xp(world.troops[static_cast<std::size_t>(world.player_troop_no)],
                           xp_amount);
    }
    if (!world.is_valid_party(world.main_party_no)) {
        return;
    }
    const auto& main_p = world.parties[static_cast<std::size_t>(world.main_party_no)];
    std::size_t num_non_prisoner = 0;
    while (num_non_prisoner < main_p.stacks.size() && !main_p.stacks[num_non_prisoner].is_prisoner()) {
        ++num_non_prisoner;
    }
    if (num_non_prisoner <= 1U) {
        return;
    }
    std::int32_t hero_count = 0;
    for (std::size_t i = 1; i < num_non_prisoner; ++i) {
        const auto& st = main_p.stacks[i];
        if (world.is_valid_troop(st.troop_no) &&
            world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
            ++hero_count;
        }
    }
    if (hero_count <= 0) {
        return;
    }
    const std::int32_t share = xp_amount / hero_count;
    if (share <= 0) {
        return;
    }
    for (std::size_t i = 1; i < num_non_prisoner; ++i) {
        const auto& st = main_p.stacks[i];
        if (world.is_valid_troop(st.troop_no) &&
            world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
            game::troop_add_xp(world.troops[static_cast<std::size_t>(st.troop_no)], share);
        }
    }
}

[[nodiscard]] inline std::int32_t native_campaign_spawn_around_party(
    std::int32_t src_party_no,
    std::int32_t template_no,
    VmContext& ctx) {
    auto& world = ctx.world;
    if (!world.is_valid_party(src_party_no) || !world.is_valid_party_template(template_no)) {
        return -1;
    }
    const auto& src = world.parties[static_cast<std::size_t>(src_party_no)];
    const auto& tmpl = world.party_templates[static_cast<std::size_t>(template_no)];
    game::WseParty spawned{};
    spawned.active = true;
    spawned.name = tmpl.name;
    spawned.flags = tmpl.flags;
    spawned.icon_no = tmpl.icon_no;
    spawned.faction_no = tmpl.faction_no;
    spawned.personality = tmpl.personality;
    spawned.template_no = template_no;
    spawned.position = src.position;
    const float angle = native_entity_randf(ctx) * 6.2831853f;
    const float dist = native_entity_randf(ctx) * std::max(0.0f, world.spawn_radius);
    spawned.position.o.x += std::cos(angle) * dist;
    spawned.position.o.y += std::sin(angle) * dist;
    const std::int32_t new_id = world.allocate_party(std::move(spawned));
    if (new_id < 0) {
        return -1;  // `kMaxPartyKeys` reached
    }
    ++world.party_templates[static_cast<std::size_t>(template_no)].num_created;
    game::party_add_template(world, new_id, template_no, false,
                             [&ctx](std::int32_t m) noexcept { return native_rgl_rand(ctx, m); });
    return new_id;
}

[[nodiscard]] inline game::WsePartyStack* native_campaign_prisoner_stack_at(
    game::WseParty& party,
    std::int32_t stack_no) noexcept {
    if (stack_no < 0) {
        return nullptr;
    }
    std::int32_t seen = 0;
    for (auto& st : party.stacks) {
        if (st.is_prisoner()) {
            if (seen == stack_no) {
                return &st;
            }
            ++seen;
        }
    }
    return nullptr;
}

[[nodiscard]] inline NativeDispatch dispatch_native_campaign_statement(
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
    // Campaign Conditions & Map State (36..154)
    case 36: // entering_town (0x1401c65c8)
        return game::check_entering_town(narrow[0], world)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 37: // map_free (0x1401c6670)
        return world.map_free_state ? NativeDispatch::handled
                                    : NativeDispatch::condition_false;
    case 39: // encountered_party_is_attacker (0x1401c668e)
        return world.encountered_party_is_attacker ? NativeDispatch::handled
                                                   : NativeDispatch::condition_false;
    case 42: // conversation_screen_is_active (0x1401c66a6)
        return world.conversation_screen_active ? NativeDispatch::handled
                                                : NativeDispatch::condition_false;
    case 44: // in_meta_mission (0x1401c66bc)
        return world.in_meta_mission ? NativeDispatch::handled
                                     : NativeDispatch::condition_false;
    case 60: { // set_result_string (0x1401c67e0)
        const std::uint8_t tag0 = tags.empty() ? 0U : tags[0];
        world.result_string =
            wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx));
        return NativeDispatch::handled;
    }
    case 101: { // hero_can_join (0x1401c6c51)
        const std::int32_t party_no =
            (statement.numOperands >= 1) ? narrow[0] : world.main_party_no;
        if (!world.is_valid_party(party_no)) {
            return NativeDispatch::condition_false;
        }
        if (party_no != world.main_party_no) {
            return NativeDispatch::handled;
        }
        const auto& p = world.parties[static_cast<std::size_t>(party_no)];
        return (party_max_companions(ctx, party_no) > p.get_num_companions())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }
    case 102: { // hero_can_join_as_prisoner (0x1401c6ca4)
        const std::int32_t party_no =
            (statement.numOperands >= 1) ? narrow[0] : world.main_party_no;
        if (!world.is_valid_party(party_no)) {
            return NativeDispatch::condition_false;
        }
        if (party_no != world.main_party_no) {
            return NativeDispatch::handled;
        }
        const auto& p = world.parties[static_cast<std::size_t>(party_no)];
        return (party_max_prisoners(ctx, party_no) > p.get_num_prisoners())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }
    case 103: // party_can_join (0x1401c6cf7)
        return (world.is_valid_party(world.encountered_party_no) &&
                world.is_valid_party(world.main_party_no) &&
                game::party_can_join_party(world, world.encountered_party_no,
                                           world.main_party_no, false,
                                           party_max_companions(ctx, world.main_party_no),
                                           party_max_prisoners(ctx, world.main_party_no)))
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 104: // party_can_join_as_prisoner (0x1401c6d20)
        return (world.is_valid_party(world.encountered_party_no) &&
                world.is_valid_party(world.main_party_no) &&
                game::party_can_join_party(world, world.encountered_party_no,
                                           world.main_party_no, true,
                                           party_max_companions(ctx, world.main_party_no),
                                           party_max_prisoners(ctx, world.main_party_no)))
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 105: { // troops_can_join (0x1401c6d33)
        if (narrow[0] <= 0 || !world.is_valid_party(world.main_party_no)) {
            return NativeDispatch::condition_false;
        }
        const auto& p = world.parties[static_cast<std::size_t>(world.main_party_no)];
        return (p.get_num_companions() + narrow[0] <= party_max_companions(ctx, world.main_party_no))
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }
    case 106: { // troops_can_join_as_prisoner (0x1401c6d6d)
        if (narrow[0] <= 0 || !world.is_valid_party(world.main_party_no)) {
            return NativeDispatch::condition_false;
        }
        const auto& p = world.parties[static_cast<std::size_t>(world.main_party_no)];
        return (p.get_num_prisoners() + narrow[0] <= party_max_prisoners(ctx, world.main_party_no))
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }
    case 107: // party_can_join_party (0x1401c6da7)
        return game::party_can_join_party(
                   world, narrow[0], narrow[1],
                   (statement.numOperands >= 3 && narrow[2] != 0),
                   party_max_companions(ctx, narrow[1]), party_max_prisoners(ctx, narrow[1]))
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 108: // party_end_battle (0x1401c6dfc)
        if (world.is_valid_party(narrow[0])) {
            auto& p = world.parties[static_cast<std::size_t>(narrow[0])];
            if (p.in_battle) {
                p.in_battle = false;
                p.battle_opponent_no = -1;
            }
        }
        return NativeDispatch::handled;
    case 130: // party_is_in_town (0x1401c6ebb)
        return (world.is_valid_party(narrow[0]) && world.is_valid_party(narrow[1]) &&
                world.parties[static_cast<std::size_t>(narrow[0])].behavior == 8 &&
                world.parties[static_cast<std::size_t>(narrow[0])].behavior_object == narrow[1])
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 131: // party_is_in_any_town (0x1401c6f02)
        return (world.is_valid_party(narrow[0]) &&
                world.parties[static_cast<std::size_t>(narrow[0])].behavior == 8)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 132: // party_is_active (0x1401c6f33)
        return (world.is_valid_party(narrow[0]) &&
                (world.parties[static_cast<std::size_t>(narrow[0])].flags &
                 0x1080100ULL) == 0ULL)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 150: // player_has_item (0x1401c6f77)
        return (world.is_valid_troop(world.player_troop_no) &&
                world.is_valid_item(narrow[0]) &&
                game::troop_count_items(world, world.player_troop_no, narrow[0], false) > 0)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 151: { // troop_has_item_equipped (0x1401c6fdd)
        if (!world.is_valid_troop(narrow[0]) || !world.is_valid_item(narrow[1])) {
            return NativeDispatch::condition_false;
        }
        const auto& tr = world.troops[static_cast<std::size_t>(narrow[0])];
        for (std::size_t i = 0; i < game::kNumTroopEquipmentSlots; ++i) {
            if (tr.inventory[i].item_no == narrow[1]) {
                return NativeDispatch::handled;
            }
        }
        return NativeDispatch::condition_false;
    }
    case 152: // troop_is_mounted (0x1401c704b)
        return (world.is_valid_troop(narrow[0]) &&
                world.troops[static_cast<std::size_t>(narrow[0])].inventory[8].item_no >= 0)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 153: // troop_is_guarantee_ranged (0x1401c7096)
        return (world.is_valid_troop(narrow[0]) &&
                (world.troops[static_cast<std::size_t>(narrow[0])].flags & (1ULL << 26U)) !=
                    0ULL)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 154: // troop_is_guarantee_horse (0x1401c70d1)
        return (world.is_valid_troop(narrow[0]) &&
                (world.troops[static_cast<std::size_t>(narrow[0])].flags & (1ULL << 24U)) !=
                    0ULL)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;

    // Platform / Achievements / Multiplayer / Teams (250..461)
    case 250: // is_trial_version (0x1401d5616)
        return world.is_trial_version ? NativeDispatch::handled
                                      : NativeDispatch::condition_false;
    case 255: // is_edit_mode_enabled (0x1401c71ed)
        return world.edit_mode_enabled ? NativeDispatch::handled
                                       : NativeDispatch::condition_false;
    case 370: { // get_achievement_stat (0x1401c75ea)
        const std::int32_t key = native_campaign_achievement_key(narrow[1], narrow[2]);
        const auto it = world.achievement_stats.find(key);
        store_lvalue(dst, (it != world.achievement_stats.end()) ? it->second : 0, ctx,
                     locals);
        return NativeDispatch::handled;
    }
    case 371: // set_achievement_stat (0x1401c761f)
        world.achievement_stats[native_campaign_achievement_key(narrow[0], narrow[1])] =
            narrow[2];
        return NativeDispatch::handled;
    case 372: // unlock_achievement (0x1401c7636)
        if (narrow[0] >= 0 && narrow[0] < 81) {
            world.unlocked_achievements[narrow[0]] = true;
        }
        return NativeDispatch::handled;
    case 411: // multiplayer_get_my_team (0x1401c83c3)
        if (world.is_multiplayer && world.is_valid_player(world.my_player_no)) {
            store_lvalue(
                dst,
                world.players[static_cast<std::size_t>(world.my_player_no)].team_no, ctx,
                locals);
        }
        return NativeDispatch::handled;
    case 412: // multiplayer_get_my_troop (0x1401c8417)
        if (world.is_multiplayer && world.is_valid_player(world.my_player_no)) {
            store_lvalue(
                dst,
                world.players[static_cast<std::size_t>(world.my_player_no)].troop_no, ctx,
                locals);
        }
        return NativeDispatch::handled;
    case 413: // multiplayer_set_my_troop (0x1401c846b)
        if (world.is_multiplayer && world.is_valid_player(world.my_player_no) &&
            (narrow[0] == -1 || world.is_valid_troop(narrow[0]))) {
            world.players[static_cast<std::size_t>(world.my_player_no)].troop_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 414: // multiplayer_get_my_gold (0x1401c84dc)
        if (world.is_multiplayer && world.is_valid_player(world.my_player_no)) {
            store_lvalue(
                dst,
                world.players[static_cast<std::size_t>(world.my_player_no)].gold, ctx,
                locals);
        }
        return NativeDispatch::handled;
    case 415: // multiplayer_get_my_player (0x1401c8530)
        if (world.is_multiplayer) {
            store_lvalue(dst, static_cast<std::int64_t>(world.my_player_no), ctx, locals);
        }
        return NativeDispatch::handled;
    case 416: // multiplayer_clear_scene (0x1401c858f)
        if (world.is_multiplayer) {
            world.clear_multiplayer_scene();
        }
        return NativeDispatch::handled;
    case 417: // multiplayer_is_server (0x1401c85e9)
        return world.is_server ? NativeDispatch::handled : NativeDispatch::condition_false;
    case 418: // multiplayer_is_dedicated_server (0x1401c85fa)
        return world.is_dedicated_server ? NativeDispatch::handled
                                         : NativeDispatch::condition_false;
    case 419: // game_in_multiplayer_mode (0x1401c860b)
        return world.is_multiplayer ? NativeDispatch::handled
                                    : NativeDispatch::condition_false;
    case 420: // multiplayer_make_everyone_enemy (0x1401c8620)
        for (std::size_t i = 0; i < world.teams.size(); ++i) {
            for (std::size_t j = 0; j < world.teams[i].relations.size(); ++j) {
                world.teams[i].relations[j] = (i == j) ? 1 : -1;
            }
        }
        return NativeDispatch::handled;
    case 421: // player_control_agent (0x1401c8645)
        if (world.is_valid_player(narrow[0]) && world.is_valid_agent(narrow[1])) {
            auto& pl = world.players[static_cast<std::size_t>(narrow[0])];
            if (world.is_valid_agent(pl.agent_no)) {
                auto& prev = world.agents[static_cast<std::size_t>(pl.agent_no)];
                prev.player_no = -1;
                prev.is_non_player = true;
            }
            auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            pl.agent_no = narrow[1];
            ag.player_no = narrow[0];
            ag.is_non_player = false;
        }
        return NativeDispatch::handled;
    case 422: // player_get_item_id (0x1401c8680)
        if (world.is_valid_player(narrow[1]) && narrow[2] >= 0 &&
            narrow[2] < game::kNumTroopEquipmentSlots) {
            store_lvalue(
                dst,
                world.players[static_cast<std::size_t>(narrow[1])]
                    .item_ids[static_cast<std::size_t>(narrow[2])],
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 423: // player_get_banner_id (0x1401c86e4)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(
                dst, world.players[static_cast<std::size_t>(narrow[1])].banner_no, ctx,
                locals);
        }
        return NativeDispatch::handled;
    case 424: // game_get_reduce_campaign_ai (0x1401c872b)
        store_lvalue(dst, static_cast<std::int64_t>(world.reduce_campaign_ai), ctx, locals);
        return NativeDispatch::handled;
    case 432: // player_set_score (0x1401c8901)
        if (world.is_valid_player(narrow[0])) {
            world.players[static_cast<std::size_t>(narrow[0])].score = narrow[1];
        }
        return NativeDispatch::handled;
    case 450: // team_get_bot_kill_count (0x1401c8be7)
        if (world.is_valid_team(narrow[1])) {
            store_lvalue(dst, world.teams[static_cast<std::size_t>(narrow[1])].bot_kill_count,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 451: // team_set_bot_kill_count (0x1401c8c2b)
        if (world.is_valid_team(narrow[0])) {
            world.teams[static_cast<std::size_t>(narrow[0])].bot_kill_count = narrow[1];
        }
        return NativeDispatch::handled;
    case 452: // team_get_bot_death_count (0x1401c8c57)
        if (world.is_valid_team(narrow[1])) {
            store_lvalue(dst, world.teams[static_cast<std::size_t>(narrow[1])].bot_death_count,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 453: // team_set_bot_death_count (0x1401c8c9b)
        if (world.is_valid_team(narrow[0])) {
            world.teams[static_cast<std::size_t>(narrow[0])].bot_death_count = narrow[1];
        }
        return NativeDispatch::handled;
    case 454: // team_get_kill_count (0x1401c8cd2)
        if (world.is_valid_team(narrow[1])) {
            store_lvalue(dst, world.teams[static_cast<std::size_t>(narrow[1])].kill_count, ctx,
                         locals);
        }
        return NativeDispatch::handled;
    case 455: // team_get_score (0x1401c8d15)
        if (world.is_valid_team(narrow[1])) {
            store_lvalue(dst, world.teams[static_cast<std::size_t>(narrow[1])].score, ctx,
                         locals);
        }
        return NativeDispatch::handled;
    case 456: // team_set_score (0x1401c8d59)
        if (world.is_valid_team(narrow[0])) {
            world.teams[static_cast<std::size_t>(narrow[0])].score = narrow[1];
        }
        return NativeDispatch::handled;
    case 457: // team_set_faction (0x1401c8d85)
        if (narrow[0] >= 0 && narrow[0] < 2 && world.is_valid_faction(narrow[1])) {
            world.teams[static_cast<std::size_t>(narrow[0])].faction_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 458: // team_get_faction (0x1401c8de8)
        if (narrow[1] >= 0 && narrow[1] < 2) {
            store_lvalue(dst, world.teams[static_cast<std::size_t>(narrow[1])].faction_no, ctx,
                         locals);
        }
        return NativeDispatch::handled;
    case 461: // player_item_slot_is_picked_up (0x1401c8efd)
        return (world.is_valid_player(narrow[0]) && narrow[1] >= 0 &&
                narrow[1] < game::kNumTroopEquipmentSlots &&
                world.players[static_cast<std::size_t>(narrow[0])]
                    .picked_up_slots[static_cast<std::size_t>(narrow[1])])
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;

    // Position Distances (712..793)
    case 712: { // get_sq_distance_between_positions (0x1401caf6a)
        std::int64_t out = 0;
        if (wse_register_ok(narrow[1]) && wse_register_ok(narrow[2])) {
            const auto& a = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            const auto& b = ctx.positions[static_cast<std::size_t>(narrow[2])].o;
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            out = game::native_f2i_saturate((dx * dx + dy * dy + dz * dz) * 100.0f);
        }
        store_lvalue(dst, out, ctx, locals);
        return NativeDispatch::handled;
    }
    case 713: { // get_sq_distance_between_positions_in_meters (0x1401cb004)
        std::int64_t out = 0;
        if (wse_register_ok(narrow[1]) && wse_register_ok(narrow[2])) {
            const auto& a = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            const auto& b = ctx.positions[static_cast<std::size_t>(narrow[2])].o;
            const float dx = a.x - b.x;
            const float dy = a.y - b.y;
            const float dz = a.z - b.z;
            out = game::native_f2i_saturate(dx * dx + dy * dy + dz * dz);
        }
        store_lvalue(dst, out, ctx, locals);
        return NativeDispatch::handled;
    }
    case 792: // position_get_distance_to_terrain (0x1401cbf73)
    case 793: // position_get_distance_to_ground_level (0x1401cc005)
        if (wse_register_ok(narrow[1])) {
            const float z = ctx.positions[static_cast<std::size_t>(narrow[1])].o.z;
            const float fp = static_cast<float>(ctx.fixedPointMultiplier);
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             f2i_round((z - world.ground_level_z) * fp)),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    // Mission State & World Campaign Ops (1003..1493)
    case 1003: // all_enemies_defeated (0x1401cd4fd)
        return native_campaign_all_enemies_defeated(world) ? NativeDispatch::handled
                                                           : NativeDispatch::condition_false;
    case 1005: // num_active_teams_le (0x1401cd511)
        return (native_campaign_num_active_teams(world) <= narrow[0])
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1006: // main_hero_fallen (0x1401cd52b)
        return (world.is_valid_agent(world.player_agent_no) &&
                !world.agents[static_cast<std::size_t>(world.player_agent_no)].is_alive())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1019: // finish_party_battle_mode (0x1401cd56b)
        world.party_battle_mode = false;
        return NativeDispatch::handled;
    case 1020: // set_party_battle_mode (0x1401cd577)
        world.party_battle_mode = true;
        return NativeDispatch::handled;
    case 1021: // set_camera_follow_party (0x1401cd583)
        if (world.is_valid_party(narrow[0])) {
            world.camera_follow_party_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 1062: { // add_xp_to_troop (0x1401cd742)
        const std::int32_t troop_no =
            (statement.numOperands >= 2) ? narrow[1] : world.player_troop_no;
        if (world.is_valid_troop(troop_no)) {
            game::troop_add_xp(world.troops[static_cast<std::size_t>(troop_no)], narrow[0]);
        }
        return NativeDispatch::handled;
    }
    case 1063: { // add_gold_as_xp (0x1401cd76c)
        const std::int32_t troop_no =
            (statement.numOperands >= 2) ? narrow[1] : world.player_troop_no;
        if (world.is_valid_troop(troop_no) && narrow[0] > 0) {
            const float mult =
                0.5f + static_cast<float>(native_rgl_rand(ctx, 15817)) / 15817.0f;
            // Safety deviation: clamp before the cast; troop_add_xp caps a single
            // award at 29999 anyway.
            const auto xp = static_cast<std::int32_t>(
                std::min(static_cast<double>(narrow[0]) * static_cast<double>(mult), 29999.0));
            game::troop_add_xp(world.troops[static_cast<std::size_t>(troop_no)], xp);
        }
        return NativeDispatch::handled;
    }
    case 1064: // add_xp_as_reward (0x1401cd780)
        native_campaign_add_xp_reward(narrow[0], world);
        return NativeDispatch::handled;
    case 1080: { // set_party_creation_random_limits (0x1401cd794)
        const std::int32_t lo = std::clamp(narrow[0], 0, 100);
        const std::int32_t hi = std::clamp(narrow[1], 0, 100);
        if (lo <= hi) {
            world.party_creation_min_random = lo;
            world.party_creation_max_random = hi;
        }
        return NativeDispatch::handled;
    }
    case 1095: // troop_set_note_available (0x1401cd9d0)
        if (world.is_valid_troop(narrow[0])) {
            world.troops[static_cast<std::size_t>(narrow[0])].note_available = (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1096: // faction_set_note_available (0x1401cda07)
        if (world.is_valid_faction(narrow[0])) {
            world.factions[static_cast<std::size_t>(narrow[0])].note_available =
                (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1097: // party_set_note_available (0x1401cda3e)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].note_available = (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1100: // spawn_around_party (0x1401cdaac)
        ctx.registers[0] = native_campaign_spawn_around_party(narrow[0], narrow[1], ctx);
        return NativeDispatch::handled;
    case 1103: // set_spawn_radius (0x1401cdb25)
        world.spawn_radius = static_cast<float>(wide[0]);
        return NativeDispatch::handled;
    case 1105: { // display_log_message (0x1401cdb3a)
        const std::uint8_t tag0 = tags.empty() ? 0U : tags[0];
        const std::u16string msg = native_resolve_and_parse(narrow[0], tag0, ctx);
        const std::uint32_t col =
            (statement.numOperands >= 2 && wide[1] != 0)
                ? static_cast<std::uint32_t>(wide[1])
                : 0xffffffffu;
        world.displayed_messages.push_back({msg, col});
        return NativeDispatch::handled;
    }
    case 1117: // add_troop_note_from_sreg (0x1401ce30b)
        if (world.is_valid_troop(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kNumQuestNotes) {
            const std::uint8_t tag2 = (tags.size() > 2) ? tags[2] : 0U;
            auto& note = world.troops[static_cast<std::size_t>(narrow[0])]
                             .notes[static_cast<std::size_t>(narrow[1])];
            note.text = native_resolve_and_parse(narrow[2], tag2, ctx);
            const bool show = (statement.numOperands < 4 || narrow[3] != 0);
            note.date = show ? world.current_day : -1;
        }
        return NativeDispatch::handled;
    case 1170: // reset_price_rates (0x1401ceb15)
        for (auto& item : world.items) {
            item.price_rate = 1.0f;
        }
        return NativeDispatch::handled;
    case 1171: // set_price_rate_for_item (0x1401ceb26)
        if (world.is_valid_item(narrow[0])) {
            world.items[static_cast<std::size_t>(narrow[0])].price_rate =
                static_cast<float>(narrow[1]) * 0.01f;
        }
        return NativeDispatch::handled;
    case 1172: { // set_price_rate_for_item_type (0x1401ceb68)
        const float rate = static_cast<float>(narrow[1]) * 0.01f;
        for (auto& item : world.items) {
            if (item.get_type() == narrow[0]) {
                item.price_rate = rate;
            }
        }
        return NativeDispatch::handled;
    }
    case 1201: // party_join (0x1401ceb8c)
        if (world.is_valid_party(world.main_party_no) &&
            world.is_valid_party(world.encountered_party_no)) {
            game::party_join_to_party(world, world.main_party_no,
                                      world.encountered_party_no, false);
        }
        return NativeDispatch::handled;
    case 1202: // party_join_as_prisoner (0x1401cec3e)
        if (world.is_valid_party(world.main_party_no) &&
            world.is_valid_party(world.encountered_party_no)) {
            game::party_join_to_party(world, world.main_party_no,
                                      world.encountered_party_no, true);
        }
        return NativeDispatch::handled;
    case 1203: // troop_join (0x1401cecf0)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_party(world.main_party_no)) {
            game::party_add_members(world, world.main_party_no, narrow[0], 1, false, 0);
        }
        return NativeDispatch::handled;
    case 1204: // troop_join_as_prisoner (0x1401cedc4)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_party(world.main_party_no)) {
            game::party_add_members(world, world.main_party_no, narrow[0], 1, true, 0);
        }
        return NativeDispatch::handled;
    case 1210: { // remove_member_from_party (0x1401cee98)
        const std::int32_t party_no =
            (statement.numOperands >= 2) ? narrow[1] : world.main_party_no;
        if (world.is_valid_troop(narrow[0]) && world.is_valid_party(party_no)) {
            game::party_remove_members(world, party_no, narrow[0], 1, false, false);
        }
        return NativeDispatch::handled;
    }
    case 1211: // remove_regular_prisoners (0x1401cefbe)
        if (world.is_valid_party(narrow[0])) {
            auto& stacks = world.parties[static_cast<std::size_t>(narrow[0])].stacks;
            stacks.erase(std::remove_if(stacks.begin(), stacks.end(),
                                        [&](const game::WsePartyStack& st) {
                                            return st.is_prisoner() &&
                                                   world.is_valid_troop(st.troop_no) &&
                                                   !world.troops[static_cast<std::size_t>(
                                                                     st.troop_no)]
                                                        .is_hero();
                                        }),
                         stacks.end());
        }
        return NativeDispatch::handled;
    case 1215: // remove_troops_from_companions (0x1401cf0ce)
        ctx.registers[0] =
            (world.is_valid_troop(narrow[0]) && narrow[1] > 0)
                ? game::party_remove_members(world, world.main_party_no, narrow[0],
                                             narrow[1], false, false)
                : 0;
        return NativeDispatch::handled;
    case 1216: // remove_troops_from_prisoners (0x1401cf245)
        ctx.registers[0] =
            (world.is_valid_troop(narrow[0]) && narrow[1] > 0)
                ? game::party_remove_members(world, world.main_party_no, narrow[0],
                                             narrow[1], true, false)
                : 0;
        return NativeDispatch::handled;
    case 1225: // heal_party (0x1401cf3bc)
        if (world.is_valid_party(narrow[0])) {
            auto& p = world.parties[static_cast<std::size_t>(narrow[0])];
            for (auto& st : p.stacks) {
                st.num_wounded_troops = 0;
                if (world.is_valid_troop(st.troop_no) &&
                    world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
                    auto& tr = world.troops[static_cast<std::size_t>(st.troop_no)];
                    tr.hit_points = static_cast<float>(
                        tr.max_hit_points(world.skills[5].max_level));
                }
            }
        }
        return NativeDispatch::handled;
    case 1230: // disable_party (0x1401cf4ef)
        if (narrow[0] >= 0 &&
            static_cast<std::size_t>(narrow[0]) < world.parties.size()) {
            world.parties[static_cast<std::size_t>(narrow[0])].flags |= 0x100ULL;
        }
        return NativeDispatch::handled;
    case 1231: // enable_party (0x1401cf587)
        if (narrow[0] >= 0 &&
            static_cast<std::size_t>(narrow[0]) < world.parties.size()) {
            world.parties[static_cast<std::size_t>(narrow[0])].flags &= ~0x100ULL;
        }
        return NativeDispatch::handled;
    case 1233: { // add_companion_party (0x1401cf644)
        ctx.registers[0] = -1;
        if (world.is_valid_troop(narrow[0]) && world.is_valid_party(world.main_party_no)) {
            const auto& main_p = world.parties[static_cast<std::size_t>(world.main_party_no)];
            const auto& tr = world.troops[static_cast<std::size_t>(narrow[0])];
            game::WseParty np{};
            np.active = true;
            np.name = tr.name;
            np.faction_no = tr.faction_no;
            np.position = main_p.position;
            np.stacks.push_back(game::WsePartyStack{narrow[0], 1, 0, 0});
            const std::int32_t new_id = world.allocate_party(std::move(np));
            ctx.registers[0] = new_id;
        }
        return NativeDispatch::handled;
    }
    case 1250: // add_troop_to_site (0x1401cf774)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_site(narrow[1])) {
            auto& visitors = world.sites[static_cast<std::size_t>(narrow[1])].visitors;
            visitors.erase(
                std::remove_if(visitors.begin(), visitors.end(),
                               [&](const game::WseSiteVisitor& v) {
                                   return v.entry_no == narrow[2] || v.troop_no == narrow[0];
                               }),
                visitors.end());
            visitors.push_back(game::WseSiteVisitor{narrow[2], narrow[0], -0xd0cd});
        }
        return NativeDispatch::handled;
    case 1251: // remove_troop_from_site (0x1401cf7aa)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_site(narrow[1])) {
            auto& visitors = world.sites[static_cast<std::size_t>(narrow[1])].visitors;
            visitors.erase(
                std::remove_if(visitors.begin(), visitors.end(),
                               [&](const game::WseSiteVisitor& v) {
                                   return v.troop_no == narrow[0];
                               }),
                visitors.end());
        }
        return NativeDispatch::handled;
    case 1265: // add_visitors_to_current_scene (0x1401cf959)
        if (world.is_valid_site(world.current_site_no) && narrow[0] >= 0 &&
            narrow[0] < game::kMaxEntryPoints && world.is_valid_troop(narrow[1]) &&
            narrow[2] > 0) {
            if (!world.is_multiplayer) {
                const std::int32_t t = (statement.numOperands >= 4) ? narrow[3] : 0;
                const std::int32_t g = (statement.numOperands >= 5) ? narrow[4] : -1;
                if (t >= 2 || (g != -1 && (g < 0 || g >= game::kNumDivisions))) {
                    return NativeDispatch::handled;
                }
            }
            auto& visitors =
                world.sites[static_cast<std::size_t>(world.current_site_no)].visitors;
            // Safety deviation: the visitor list is capped at kMaxSiteVisitors.
            for (std::int32_t k = 0;
                 k < narrow[2] && visitors.size() < game::kMaxSiteVisitors; ++k) {
                visitors.push_back(game::WseSiteVisitor{narrow[0], narrow[1], -0xd0cd});
            }
        }
        return NativeDispatch::handled;
    case 1266: // scene_set_day_time (0x1401cfa3c)
        world.scene_day_time = static_cast<float>(std::clamp(narrow[0], 0, 23));
        return NativeDispatch::handled;
    case 1492: // reset_item_probabilities (0x1401d0167)
        for (auto& item : world.items) {
            item.probability = 100.0f;
        }
        return NativeDispatch::handled;
    case 1493: // set_item_probability_in_merchandise (0x1401d0193)
        if (world.is_valid_item(narrow[0])) {
            world.items[static_cast<std::size_t>(narrow[0])].probability =
                static_cast<float>(std::max(0, narrow[1]));
        }
        return NativeDispatch::handled;

    // Troop & Item Helpers (1501..1570)
    case 1501: // troop_set_name (0x1401d01d9)
        if (world.is_valid_troop(narrow[0])) {
            const std::uint8_t tag1 = (tags.size() > 1) ? tags[1] : 0U;
            world.troops[static_cast<std::size_t>(narrow[0])].name =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    case 1502: // troop_set_plural_name (0x1401d0264)
        if (world.is_valid_troop(narrow[0])) {
            const std::uint8_t tag1 = (tags.size() > 1) ? tags[1] : 0U;
            world.troops[static_cast<std::size_t>(narrow[0])].plural_name =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    case 1508: // troop_is_wounded (0x1401d042b)
        if (!world.is_valid_troop(narrow[0])) {
            return NativeDispatch::condition_false;
        }
        {
            const auto& tr = world.troops[static_cast<std::size_t>(narrow[0])];
            return (tr.is_hero() && tr.hit_points < 10.0f)
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
    case 1510: // troop_ensure_inventory_space (0x1401d04a0 -> 0x1400e2420)
        if (world.is_valid_troop(narrow[0])) {
            game::troop_ensure_inventory_space(
                world, world.troops[static_cast<std::size_t>(narrow[0])], narrow[1],
                [&ctx](std::int32_t m) noexcept { return native_rgl_rand(ctx, m); });
        }
        return NativeDispatch::handled;
    case 1511: // troop_sort_inventory (0x1401d04ca -> 0x140247900)
        if (world.is_valid_troop(narrow[0])) {
            game::troop_sort_inventory(world,
                                       world.troops[static_cast<std::size_t>(narrow[0])]);
        }
        return NativeDispatch::handled;
    case 1512: // troop_add_merchandise (0x1401d04f7 -> 0x1400e2520)
        if (world.is_valid_troop(narrow[0])) {
            game::troop_add_merchandise(
                world, narrow[0], narrow[1], narrow[2], -1,
                [&ctx](std::int32_t m) noexcept { return native_rgl_rand(ctx, m); });
        }
        return NativeDispatch::handled;
    case 1513: // troop_add_merchandise_with_faction (0x1401d052e -> 0x1400e2520)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_faction(narrow[1])) {
            game::troop_add_merchandise(
                world, narrow[0], narrow[2], narrow[3], narrow[1],
                [&ctx](std::int32_t m) noexcept { return native_rgl_rand(ctx, m); });
        }
        return NativeDispatch::handled;
    case 1515: // troop_get_xp (0x1401d0576)
        if (world.is_valid_troop(narrow[1])) {
            store_lvalue(dst, world.troops[static_cast<std::size_t>(narrow[1])].xp, ctx,
                         locals);
        }
        return NativeDispatch::handled;
    case 1525: // troop_add_proficiency_points (0x1401d07a2)
        if (world.is_valid_troop(narrow[0])) {
            world.troops[static_cast<std::size_t>(narrow[0])].proficiency_points +=
                std::clamp(narrow[1], 0, 100);
        }
        return NativeDispatch::handled;
    case 1531: // troop_remove_item (0x1401d08d5)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_item(narrow[1])) {
            game::troop_remove_items(world, narrow[0], narrow[1], 1);
        }
        return NativeDispatch::handled;
    case 1533: // troop_equip_items (0x1401d0a0c -> 0x140246a80)
        if (world.is_valid_troop(narrow[0])) {
            game::troop_equip_items(world, world.troops[static_cast<std::size_t>(narrow[0])],
                                    -1, false, false);
        }
        return NativeDispatch::handled;
    case 1534: // troop_inventory_slot_set_item_amount (0x1401d0a43)
        if (world.is_valid_troop(narrow[0])) {
            auto* slot = native_troop_slot_ptr(
                world.troops[static_cast<std::size_t>(narrow[0])], narrow[1], world);
            if (slot != nullptr) {
                if (narrow[2] <= 0) {
                    slot->clear();
                } else {
                    slot->ammo = narrow[2];
                }
            }
        }
        return NativeDispatch::handled;
    case 1543: // troop_set_inventory_slot (0x1401d12fe)
        if (world.is_valid_troop(narrow[0]) &&
            (narrow[2] == -1 || world.is_valid_item(narrow[2]))) {
            auto* slot = native_troop_slot_ptr(
                world.troops[static_cast<std::size_t>(narrow[0])], narrow[1], world);
            if (slot != nullptr) {
                if (narrow[2] < 0) {
                    slot->clear();
                } else {
                    slot->item_no = narrow[2];
                    slot->modifier = 0;
                    slot->ammo = world.items[static_cast<std::size_t>(narrow[2])].max_ammo;
                    slot->hit_points =
                        world.items[static_cast<std::size_t>(narrow[2])].hit_points;
                }
            }
        }
        return NativeDispatch::handled;
    case 1544: // troop_set_inventory_slot_modifier (0x1401d139c)
        if (world.is_valid_troop(narrow[0]) && narrow[2] >= 0 && narrow[2] < 43) {
            auto* slot = native_troop_slot_ptr(
                world.troops[static_cast<std::size_t>(narrow[0])], narrow[1], world);
            if (slot != nullptr && !slot->is_empty()) {
                slot->modifier = narrow[2];
            }
        }
        return NativeDispatch::handled;
    case 1555: // troop_set_age (0x1401d1460)
        if (world.is_valid_troop(narrow[0])) {
            world.troops[static_cast<std::size_t>(narrow[0])].set_age(narrow[1]);
        }
        return NativeDispatch::handled;
    case 1561: // troop_get_upgrade_troop (0x1401d154b)
        if (world.is_valid_troop(narrow[1]) && (narrow[2] == 0 || narrow[2] == 1)) {
            store_lvalue(
                dst,
                world.troops[static_cast<std::size_t>(narrow[1])]
                    .upgrade_troop_nos[static_cast<std::size_t>(narrow[2])],
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1570: // item_get_type (0x1401d15dc)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst,
                         world.items[static_cast<std::size_t>(narrow[1])].get_type(),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    // Party Operations (1604..1694)
    case 1604: // party_set_marshall (0x1401d1782)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].is_marshall = (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1605: // party_set_extra_text (0x1401d17b1)
        if (world.is_valid_party(narrow[0])) {
            const std::uint8_t tag1 = (tags.size() > 1) ? tags[1] : 0U;
            world.parties[static_cast<std::size_t>(narrow[0])].extra_text =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    case 1606: // party_set_aggressiveness (0x1401d183c)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].set_aggressiveness(narrow[1]);
        }
        return NativeDispatch::handled;
    case 1607: // party_set_courage (0x1401d186d)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].set_courage(narrow[1]);
        }
        return NativeDispatch::handled;
    case 1609: // party_get_template_id (0x1401d18e5)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst, world.parties[static_cast<std::size_t>(narrow[1])].template_no,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1611: // party_add_prisoners (0x1401d1984)
        ctx.registers[0] =
            (narrow[2] > 0)
                ? game::party_add_members(world, narrow[0], narrow[1], narrow[2], true, 0)
                : 0;
        return NativeDispatch::handled;
    case 1612: // party_add_leader (0x1401d19dc)
        if (narrow[0] != world.main_party_no) {
            const std::int32_t cnt =
                (statement.numOperands >= 3 && narrow[2] > 0) ? narrow[2] : 1;
            game::party_add_leader(world, narrow[0], narrow[1], cnt);
        }
        return NativeDispatch::handled;
    case 1613: // party_force_add_members (0x1401d1a4c)
        if (world.is_valid_party(narrow[0]) && world.is_valid_troop(narrow[1]) &&
            narrow[2] > 0) {
            world.parties[static_cast<std::size_t>(narrow[0])].add_members(
                narrow[1], narrow[2], false);
        }
        return NativeDispatch::handled;
    case 1614: // party_force_add_prisoners (0x1401d1aa6)
        if (world.is_valid_party(narrow[0]) && world.is_valid_troop(narrow[1]) &&
            narrow[2] > 0) {
            world.parties[static_cast<std::size_t>(narrow[0])].add_members(
                narrow[1], narrow[2], true);
        }
        return NativeDispatch::handled;
    case 1616: // party_remove_prisoners (0x1401d1b6a)
        ctx.registers[0] =
            (narrow[2] > 0)
                ? game::party_remove_members(world, narrow[0], narrow[1], narrow[2], true,
                                             false)
                : 0;
        return NativeDispatch::handled;
    case 1617: // party_clear (0x1401d1bce)
        if (world.is_valid_party(narrow[0])) {
            auto& p = world.parties[static_cast<std::size_t>(narrow[0])];
            if (narrow[0] != world.main_party_no && p.is_bot()) {
                p.stacks.clear();
            } else if (p.stacks.size() > 1U) {
                p.stacks.resize(1U);
            }
        }
        return NativeDispatch::handled;
    case 1618: // party_wound_members (0x1401d1c96)
        if (world.is_valid_party(narrow[0]) && world.is_valid_troop(narrow[1]) &&
            narrow[2] > 0) {
            auto& p = world.parties[static_cast<std::size_t>(narrow[0])];
            for (auto& st : p.stacks) {
                if (!st.is_prisoner() && st.troop_no == narrow[1]) {
                    if (!world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
                        st.num_wounded_troops =
                            std::clamp(st.num_wounded_troops + narrow[2], 0, st.num_troops);
                    }
                    break;
                }
            }
        }
        return NativeDispatch::handled;
    case 1619: // party_remove_members_wounded_first (0x1401d1da1)
        ctx.registers[0] =
            (narrow[2] > 0)
                ? game::party_remove_members(world, narrow[0], narrow[1], narrow[2], false,
                                             true)
                : 0;
        return NativeDispatch::handled;
    case 1623: // party_relocate_near_party (0x1401d1eb4)
        if (world.is_valid_party(narrow[0]) && world.is_valid_party(narrow[1])) {
            const float radius = static_cast<float>(std::max<std::int64_t>(0, wide[2]));
            const float angle = native_entity_randf(ctx) * 6.2831853f;
            const float dist = native_entity_randf(ctx) * radius;
            auto pos = world.parties[static_cast<std::size_t>(narrow[1])].position;
            pos.o.x += std::cos(angle) * dist;
            pos.o.y += std::sin(angle) * dist;
            world.parties[static_cast<std::size_t>(narrow[0])].position = pos;
        }
        return NativeDispatch::handled;
    case 1627: // map_get_random_position_around_position (0x1401d206c)
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1]) && wide[2] >= 0) {
            const float radius = static_cast<float>(wide[2]);
            const float angle = native_entity_randf(ctx) * 6.2831853f;
            const float dist = native_entity_randf(ctx) * radius;
            auto pos = ctx.positions[static_cast<std::size_t>(narrow[1])];
            pos.o.x += std::cos(angle) * dist;
            pos.o.y += std::sin(angle) * dist;
            ctx.positions[static_cast<std::size_t>(narrow[0])] = pos;
        }
        return NativeDispatch::handled;
    case 1630: { // party_count_members_of_type (0x1401d2209)
        if (world.is_valid_party(narrow[1]) && world.is_valid_troop(narrow[2])) {
            const auto& p = world.parties[static_cast<std::size_t>(narrow[1])];
            const std::int32_t total =
                game::party_count_troops(p, narrow[2], false) +
                game::party_count_troops(p, narrow[2], true);
            store_lvalue(dst, static_cast<std::int64_t>(total), ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case 1631: // party_count_companions_of_type (0x1401d2280)
        if (world.is_valid_party(narrow[1]) && world.is_valid_troop(narrow[2])) {
            store_lvalue(dst,
                         game::party_count_troops(
                             world.parties[static_cast<std::size_t>(narrow[1])], narrow[2],
                             false),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1632: // party_count_prisoners_of_type (0x1401d22dc)
        if (world.is_valid_party(narrow[1]) && world.is_valid_troop(narrow[2])) {
            store_lvalue(dst,
                         game::party_count_troops(
                             world.parties[static_cast<std::size_t>(narrow[1])], narrow[2],
                             true),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1633: // party_get_free_companions_capacity (0x1401d2338)
        if (world.is_valid_party(narrow[1])) {
            const auto& p = world.parties[static_cast<std::size_t>(narrow[1])];
            const std::int32_t free_cap =
                std::max(0, party_max_companions(ctx, narrow[1]) - p.get_num_companions());
            store_lvalue(dst, static_cast<std::int64_t>(free_cap), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1634: // party_get_free_prisoners_capacity (0x1401d239b)
        if (world.is_valid_party(narrow[1])) {
            const auto& p = world.parties[static_cast<std::size_t>(narrow[1])];
            const std::int32_t free_cap =
                std::max(0, party_max_prisoners(ctx, narrow[1]) - p.get_num_prisoners());
            store_lvalue(dst, static_cast<std::int64_t>(free_cap), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1638: // party_get_ai_initiative (0x1401d23f4)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(
                             world.parties[static_cast<std::size_t>(narrow[1])].ai_initiative *
                             100.0f),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1639: // party_set_ai_initiative (0x1401d244c)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].ai_initiative =
                static_cast<float>(std::clamp(narrow[1], 0, 100)) / 100.0f;
        }
        return NativeDispatch::handled;
    case 1642: // party_set_ai_target_position (0x1401d2527)
        if (world.is_valid_party(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& o = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            world.parties[static_cast<std::size_t>(narrow[0])].default_behavior_position = {
                o.x, o.y};
        }
        return NativeDispatch::handled;
    case 1643: // party_set_ai_patrol_radius (0x1401d258a)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].patrol_radius =
                static_cast<float>(wide[1]);
        }
        return NativeDispatch::handled;
    case 1644: // party_ignore_player (0x1401d25c4)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].ignore_player_timer =
                static_cast<float>(std::max<std::int64_t>(0, wide[1]));
        }
        return NativeDispatch::handled;
    case 1645: // party_set_bandit_attraction (0x1401d260c)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].bandit_attraction =
                std::clamp(narrow[1], 0, 100);
        }
        return NativeDispatch::handled;
    case 1646: // party_get_helpfulness (0x1401d264e)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(
                             world.parties[static_cast<std::size_t>(narrow[1])].helpfulness *
                             100.0f),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1647: // party_set_helpfulness (0x1401d26a6)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].helpfulness =
                static_cast<float>(std::clamp(narrow[1], 0, 10000)) / 100.0f;
        }
        return NativeDispatch::handled;
    case 1648: // party_set_ignore_with_player_party (0x1401d26f3)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].ignore_with_player_party =
                (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1649: // party_get_ignore_with_player_party (0x1401d2722)
        if (world.is_valid_party(narrow[0])) {
            return world.parties[static_cast<std::size_t>(narrow[0])].ignore_with_player_party
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::handled;
    case 1654: // party_stack_get_num_wounded (0x1401d28d8)
        if (world.is_valid_party(narrow[1]) && narrow[2] >= 0) {
            const auto& stacks = world.parties[static_cast<std::size_t>(narrow[1])].stacks;
            if (static_cast<std::size_t>(narrow[2]) < stacks.size()) {
                store_lvalue(dst, stacks[static_cast<std::size_t>(narrow[2])].num_wounded_troops,
                             ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1655: // party_stack_get_troop_dna (0x1401d2945)
        if (world.is_valid_party(narrow[1]) && narrow[2] >= 0) {
            const auto& stacks = world.parties[static_cast<std::size_t>(narrow[1])].stacks;
            if (static_cast<std::size_t>(narrow[2]) < stacks.size()) {
                store_lvalue(dst, stacks[static_cast<std::size_t>(narrow[2])].dna, ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1656: // party_prisoner_stack_get_troop_id (0x1401d29e5)
        if (world.is_valid_party(narrow[1])) {
            if (const auto* st = native_campaign_prisoner_stack_at(
                    world.parties[static_cast<std::size_t>(narrow[1])], narrow[2])) {
                store_lvalue(dst, st->troop_no, ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1657: // party_prisoner_stack_get_size (0x1401d2a5c)
        if (world.is_valid_party(narrow[1])) {
            if (const auto* st = native_campaign_prisoner_stack_at(
                    world.parties[static_cast<std::size_t>(narrow[1])], narrow[2])) {
                store_lvalue(dst, st->num_troops, ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1658: // party_prisoner_stack_get_troop_dna (0x1401d2ad2)
        if (world.is_valid_party(narrow[1])) {
            if (const auto* st = native_campaign_prisoner_stack_at(
                    world.parties[static_cast<std::size_t>(narrow[1])], narrow[2])) {
                store_lvalue(dst, st->dna, ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1662: // party_collect_attachments_to_party (0x1401d2c07)
        game::party_collect_attachments(world, narrow[0], narrow[1]);
        return NativeDispatch::handled;
    case 1670: { // party_add_xp_to_stack (0x1401d2e54 -> 0x1401c2d30)
        if (world.is_valid_party(narrow[0]) && narrow[1] >= 0 && narrow[2] > 0) {
            const auto& stacks = world.parties[static_cast<std::size_t>(narrow[0])].stacks;
            if (static_cast<std::size_t>(narrow[1]) < stacks.size()) {
                const std::int32_t tr_no =
                    stacks[static_cast<std::size_t>(narrow[1])].troop_no;
                std::int32_t req = -1;
                if (world.is_valid_troop(tr_no)) {
                    const auto& tr = world.troops[static_cast<std::size_t>(tr_no)];
                    if (!tr.is_hero() && tr.upgrade_troop_nos[0] > 0) {
                        req = troop_upgrade_xp(ctx, tr_no);
                    }
                }
                game::party_add_xp_to_stack(world, narrow[0], narrow[1], narrow[2], req);
            }
        }
        return NativeDispatch::handled;
    }
    case 1671: // party_get_morale (0x1401d2eb0)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(
                dst,
                f2i_round(world.parties[static_cast<std::size_t>(narrow[1])].morale * 100.0f),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1672: // party_set_morale (0x1401d2f08)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].morale =
                static_cast<float>(std::clamp(narrow[1], 0, 100)) / 100.0f;
        }
        return NativeDispatch::handled;
    case 1673: // party_upgrade_with_xp (0x1401d3006 -> 0x1400df0e0)
        game::party_upgrade_with_xp(
            world, narrow[0], narrow[1], narrow[2],
            [&ctx](std::int32_t m) noexcept { return native_rgl_rand(ctx, m); },
            [&ctx](std::int32_t tr_no) { return troop_upgrade_xp(ctx, tr_no); });
        return NativeDispatch::handled;
    case 1674: // party_add_xp (0x1401d305f -> 0x1400e64e0)
        game::party_add_xp(world, narrow[0], narrow[1],
                           [&ctx](std::int32_t tr_no) { return troop_upgrade_xp(ctx, tr_no); });
        return NativeDispatch::handled;
    case 1675: // party_add_template (0x1401d30ac -> 0x1400de710)
        game::party_add_template(
            world, narrow[0], narrow[1],
            (statement.numOperands >= 3 && narrow[2] != 0),
            [&ctx](std::int32_t m) noexcept { return native_rgl_rand(ctx, m); });
        return NativeDispatch::handled;
    case 1690: // get_battle_advantage (0x1401d3550)
        store_lvalue(dst, static_cast<std::int64_t>(world.battle_advantage), ctx, locals);
        return NativeDispatch::handled;
    case 1691: // set_battle_advantage (0x1401d3577)
        world.battle_advantage = narrow[0];
        return NativeDispatch::handled;
    case 1694: // party_get_attached_to (0x1401d3622)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(
                dst,
                world.parties[static_cast<std::size_t>(narrow[1])].attached_to_party_no, ctx,
                locals);
        }
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_campaign_2_statement(opcode, wide, narrow, tags, statement, ctx,
                                                locals);
}

}  // namespace wse::vm

#include "wse/vm/native_campaign_ops_2.h"
