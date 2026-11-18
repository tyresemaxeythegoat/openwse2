#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>

#include "wse/game/world_campaign_helpers.h"
#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_world_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

[[nodiscard]] inline std::int32_t native_campaign_item_weapon_damage(
    const game::WseItemKind& item) noexcept {
    return std::max(item.get_thrust_damage(), item.get_swing_damage());
}

inline void native_campaign_deliver_damage(std::int32_t deliverer_id,
                                           std::int32_t target_id,
                                           std::int32_t raw_damage,
                                           std::int32_t item_id,
                                           bool has_item,
                                           game::WseWorldState& world) noexcept {
    if (!world.is_valid_agent(deliverer_id) || !world.is_valid_agent(target_id)) {
        return;
    }
    const auto& deliverer = world.agents[static_cast<std::size_t>(deliverer_id)];
    auto& target = world.agents[static_cast<std::size_t>(target_id)];
    if (!target.is_alive()) {
        return;
    }

    std::int32_t base_damage = 0;
    if (raw_damage > 0) {
        base_damage = std::min(raw_damage, 1000);
    } else if (has_item && world.is_valid_item(item_id)) {
        base_damage = native_campaign_item_weapon_damage(
            world.items[static_cast<std::size_t>(item_id)]);
    } else if (world.is_valid_item(deliverer.wielded_items[0])) {
        base_damage = native_campaign_item_weapon_damage(
            world.items[static_cast<std::size_t>(deliverer.wielded_items[0])]);
    }

    const float final_damage =
        std::max(0.0f, static_cast<float>(base_damage) * deliverer.damage_modifier);
    target.hit_points -= final_damage;
    if (target.hit_points <= 0.0f) {
        target.hit_points = 0.0f;
        target.status = 3;
    }
}

inline void native_campaign_unequip_item(game::WseAgent& ag,
                                         std::int32_t item_id,
                                         std::int32_t weapon_slot_no,
                                         bool has_weapon_slot) noexcept {
    if (has_weapon_slot && weapon_slot_no >= 1 && weapon_slot_no <= 4) {
        const auto idx = static_cast<std::size_t>(weapon_slot_no - 1);
        if (ag.equipped_items[idx] == item_id) {
            ag.equipped_items[idx] = -1;
            ag.item_ammo[idx] = 0;
        }
    } else {
        for (std::size_t i = 0; i < ag.equipped_items.size(); ++i) {
            if (ag.equipped_items[i] == item_id) {
                ag.equipped_items[i] = -1;
                ag.item_ammo[i] = 0;
                break;
            }
        }
    }
    for (auto& held : ag.wielded_items) {
        if (held == item_id) {
            held = -1;
        }
    }
}

inline void native_campaign_equip_item(game::WseAgent& ag,
                                       const game::WseItemKind& item,
                                       std::int32_t item_id,
                                       std::int32_t weapon_slot_no,
                                       bool has_weapon_slot) noexcept {
    const std::int32_t item_type = item.get_type();
    if (item_type >= 12 && item_type <= 15) {
        ag.equipped_items[static_cast<std::size_t>(item_type - 8)] = item_id;
        return;
    }
    if (has_weapon_slot && weapon_slot_no >= 1 && weapon_slot_no <= 4) {
        const auto idx = static_cast<std::size_t>(weapon_slot_no - 1);
        if (ag.equipped_items[idx] < 0) {
            ag.equipped_items[idx] = item_id;
            ag.item_ammo[idx] = item.max_ammo;
        }
        return;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        if (ag.equipped_items[i] < 0) {
            ag.equipped_items[i] = item_id;
            ag.item_ammo[i] = item.max_ammo;
            return;
        }
    }
}

[[nodiscard]] inline std::int32_t native_campaign_spawn_agent(
    std::int32_t troop_no,
    game::WseWorldState& world) {
    if (!world.is_valid_troop(troop_no)) {
        return -1;
    }
    const auto& tr = world.troops[static_cast<std::size_t>(troop_no)];
    game::WseAgent ag{};
    ag.active = true;
    ag.status = 1;
    ag.troop_no = troop_no;
    ag.class_no = tr.class_no;
    ag.position = world.spawn_position;
    ag.look_position = world.spawn_position;
    for (std::size_t i = 0; i < ag.equipped_items.size(); ++i) {
        ag.equipped_items[i] = tr.inventory[i].item_no;
        ag.item_ammo[i] = tr.inventory[i].ammo;
    }
    ag.horse_item_no = tr.inventory[8].item_no;
    const std::int32_t hp = tr.max_hit_points(world.skills[5].max_level);
    ag.max_hit_points = static_cast<float>(std::max(1, hp));
    ag.hit_points = ag.max_hit_points;
    return world.allocate_agent(std::move(ag));
}

[[nodiscard]] inline std::int32_t native_campaign_random_quest(
    std::int32_t lower,
    std::int32_t upper,
    VmContext& ctx) noexcept {
    const auto& world = ctx.world;
    if (world.quests.empty()) {
        return -1;
    }
    const std::int32_t lo = std::max<std::int32_t>(0, lower);
    const std::int32_t hi = std::min<std::int32_t>(
        static_cast<std::int32_t>(world.quests.size()), upper);
    const std::int32_t span = hi - lo;
    if (span <= 0) {
        return -1;
    }
    for (std::int32_t attempt = 0; attempt < 1000; ++attempt) {
        const std::int32_t q = lo + ((span > 1) ? native_rgl_rand(ctx, span) : 0);
        if (world.is_valid_quest(q)) {
            const auto& quest = world.quests[static_cast<std::size_t>(q)];
            if ((quest.template_flags & 2U) != 0U && !quest.is_active()) {
                return q;
            }
        }
    }
    return -1;
}

[[nodiscard]] inline std::int32_t native_campaign_random_troop_in_chain(
    std::int32_t root_troop,
    bool for_capture,
    VmContext& ctx) noexcept {
    const auto& world = ctx.world;
    if (!world.is_valid_troop(root_troop)) {
        return root_troop;
    }
    std::array<std::int32_t, 32> chain{};
    std::int32_t len = 0;
    std::int32_t cur = root_troop;
    while (len < static_cast<std::int32_t>(chain.size()) && world.is_valid_troop(cur)) {
        chain[static_cast<std::size_t>(len++)] = cur;
        const std::int32_t nxt =
            world.troops[static_cast<std::size_t>(cur)].upgrade_troop_nos[0];
        if (nxt <= 0 || !world.is_valid_troop(nxt) || nxt == cur) {
            break;
        }
        cur = nxt;
    }
    if (len <= 1) {
        return chain[0];
    }
    std::int32_t player_lvl = 1;
    if (world.is_valid_troop(world.player_troop_no)) {
        player_lvl = world.troops[static_cast<std::size_t>(world.player_troop_no)].level;
    }
    const std::int32_t roll_max = player_lvl / 3 + 1;
    const std::int32_t rolled = (roll_max > 1) ? native_rgl_rand(ctx, roll_max) : 0;
    const std::int32_t floor_idx = for_capture ? 0 : 1;
    const std::int32_t idx = std::min(len - 1, std::max(floor_idx, rolled + 1));
    return chain[static_cast<std::size_t>(idx)];
}

[[nodiscard]] inline std::int32_t native_campaign_random_party(
    std::int32_t lower,
    std::int32_t upper,
    VmContext& ctx) noexcept {
    const auto& world = ctx.world;
    const std::int32_t lo = std::max<std::int32_t>(0, lower);
    const std::int32_t hi = std::min<std::int32_t>(
        static_cast<std::int32_t>(world.parties.size()), upper);
    const std::int32_t span = hi - lo;
    if (span <= 0) {
        return -1;
    }
    for (std::int32_t attempt = 0; attempt < 100000; ++attempt) {
        const std::int32_t p = lo + ((span > 1) ? native_rgl_rand(ctx, span) : 0);
        if (world.is_valid_party(p) && p != world.encountered_party_no) {
            return p;
        }
    }
    return -1;
}

[[nodiscard]] inline std::int32_t native_campaign_random_item(
    std::initializer_list<std::int32_t> types,
    VmContext& ctx) noexcept {
    const auto& world = ctx.world;
    if (world.items.empty()) {
        return -1;
    }
    const auto n = static_cast<std::int32_t>(world.items.size());
    auto matches_type = [&](const game::WseItemKind& it) {
        const std::int32_t t = it.get_type();
        for (const std::int32_t allowed : types) {
            if (t == allowed) {
                return true;
            }
        }
        return false;
    };
    for (std::int32_t attempt = 0; attempt < 256; ++attempt) {
        const std::int32_t idx = (n > 1) ? native_rgl_rand(ctx, n) : 0;
        const auto& it = world.items[static_cast<std::size_t>(idx)];
        if (matches_type(it) && (it.properties & 0x04000000ULL) != 0ULL) {
            return idx;
        }
    }
    for (std::int32_t attempt = 0; attempt < 256; ++attempt) {
        const std::int32_t idx = (n > 1) ? native_rgl_rand(ctx, n) : 0;
        if (matches_type(world.items[static_cast<std::size_t>(idx)])) {
            return idx;
        }
    }
    for (std::int32_t i = 0; i < n; ++i) {
        if (matches_type(world.items[static_cast<std::size_t>(i)])) {
            return i;
        }
    }
    return -1;
}

inline void native_campaign_reset_timer(std::size_t idx, VmContext& ctx) noexcept {
    if (idx >= ctx.world.mission_timers.size()) {
        return;
    }
    auto& timer = ctx.world.mission_timers[idx];
    if (timer.timer_no >= 0 && timer.timer_no < 3) {
        timer.value = ctx.timersTotal[static_cast<std::size_t>(timer.timer_no)];
    }
}

inline void native_campaign_set_agent_mod(std::int32_t agent_id,
                                          std::int64_t raw_val,
                                          bool clamp_0_1000,
                                          float game::WseAgent::* member,
                                          game::WseWorldState& world) noexcept {
    if (!world.is_valid_agent(agent_id)) {
        return;
    }
    const std::int64_t v =
        clamp_0_1000 ? std::clamp<std::int64_t>(raw_val, 0, 1000) : raw_val;
    world.agents[static_cast<std::size_t>(agent_id)].*member =
        static_cast<float>(v) / 100.0f;
}

[[nodiscard]] inline NativeDispatch dispatch_native_campaign_2_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    auto& world = ctx.world;
    const float fp = static_cast<float>(ctx.fixedPointMultiplier);

    switch (opcode) {
    case 1695: { // party_get_num_attached_parties (0x1401d3689)
        if (world.is_valid_party(narrow[1])) {
            std::int32_t count = 0;
            for (const std::int32_t pid :
                 world.parties[static_cast<std::size_t>(narrow[1])].attached_party_nos) {
                if (world.is_valid_party(pid)) {
                    ++count;
                }
            }
            store_lvalue(dst, static_cast<std::int64_t>(count), ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case 1696: { // party_get_attached_party_with_rank (0x1401d36f1)
        if (world.is_valid_party(narrow[1])) {
            std::int32_t found = -1;
            std::int32_t rank = 0;
            if (narrow[2] >= 0) {
                for (const std::int32_t pid :
                     world.parties[static_cast<std::size_t>(narrow[1])]
                         .attached_party_nos) {
                    if (world.is_valid_party(pid)) {
                        if (rank == narrow[2]) {
                            found = pid;
                            break;
                        }
                        ++rank;
                    }
                }
            }
            store_lvalue(dst, static_cast<std::int64_t>(found), ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case 1699: // agent_is_routed (0x1401d383e)
        return (world.is_valid_agent(narrow[0]) &&
                world.agents[static_cast<std::size_t>(narrow[0])].is_routed)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1701: { // get_player_agent_kill_count (0x1401d389c)
        const bool get_wounded = (statement.numOperands >= 2 && narrow[1] != 0);
        const std::int32_t count = get_wounded ? world.player_agent_own_troop_wound_count
                                               : world.player_agent_own_troop_kill_count;
        store_lvalue(dst, static_cast<std::int64_t>(count), ctx, locals);
        return NativeDispatch::handled;
    }
    case 1709: // agent_get_look_position (0x1401d3aeb)
        if (wse_register_ok(narrow[0]) && world.is_valid_agent(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.agents[static_cast<std::size_t>(narrow[1])].look_position;
        }
        return NativeDispatch::handled;
    case 1719: // agent_get_item_id (0x1401d3f20)
        if (world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            store_lvalue(dst, static_cast<std::int64_t>(ag.is_human() ? -1 : ag.horse_item_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1722: // agent_deliver_damage_to_agent (0x1401d405f)
        native_campaign_deliver_damage(
            narrow[0], narrow[1],
            (statement.numOperands >= 3) ? narrow[2] : 0,
            (statement.numOperands >= 4) ? narrow[3] : -1,
            statement.numOperands >= 4, world);
        return NativeDispatch::handled;
    case 1724: // agent_get_player_id (0x1401d42b8)
        if (world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            store_lvalue(dst, static_cast<std::int64_t>(ag.is_non_player ? -1 : ag.player_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1737: // agent_ai_set_always_attack_in_melee (0x1401d4876)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].always_attack_in_melee =
                (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1751: // agent_start_running_away (0x1401d4eef)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            ag.running_away = true;
            if (statement.numOperands >= 2 && narrow[1] > 0 && wse_register_ok(narrow[1])) {
                ag.run_away_target =
                    ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            }
        }
        return NativeDispatch::handled;
    case 1771: // agent_set_team (0x1401d566b)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_team(narrow[1])) {
            world.agents[static_cast<std::size_t>(narrow[0])].team_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1774: // agent_unequip_item (0x1401d576c)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_item(narrow[1])) {
            native_campaign_unequip_item(
                world.agents[static_cast<std::size_t>(narrow[0])], narrow[1],
                (statement.numOperands >= 3) ? narrow[2] : 0,
                statement.numOperands >= 3);
        }
        return NativeDispatch::handled;
    case 1775: { // class_is_listening_order (0x1401d580e)
        if (!world.is_valid_team(narrow[0]) || narrow[1] < 0 ||
            narrow[1] >= game::kNumDivisions) {
            return NativeDispatch::condition_false;
        }
        const std::int32_t listener =
            world.teams[static_cast<std::size_t>(narrow[0])].order_listener;
        return (listener == narrow[1] || listener == 10)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }
    case 1779: // agent_equip_item (0x1401d59cf)
        if (world.is_valid_agent(narrow[0]) && world.is_valid_item(narrow[1])) {
            if (statement.numOperands >= 4 && (narrow[3] < 0 || narrow[3] >= 43)) {
                return NativeDispatch::handled;
            }
            native_campaign_equip_item(
                world.agents[static_cast<std::size_t>(narrow[0])],
                world.items[static_cast<std::size_t>(narrow[1])], narrow[1],
                (statement.numOperands >= 3) ? narrow[2] : 0,
                statement.numOperands >= 3);
        }
        return NativeDispatch::handled;
    case 1785: { // team_get_movement_order (0x1401d5fe4)
        if (world.is_valid_team(narrow[1])) {
            const std::int32_t div = std::clamp(narrow[2], 0, 8);
            const std::int32_t raw = world.teams[static_cast<std::size_t>(narrow[1])]
                                         .order_types[static_cast<std::size_t>(div)];
            std::int32_t out = -1;
            switch (raw) {
            case 0: out = 2; break;
            case 1: out = 0; break;
            case 2: out = 1; break;
            case 3: out = 11; break;
            case 4: out = 14; break;
            default: break;
            }
            store_lvalue(dst, static_cast<std::int64_t>(out), ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case 1788: // teams_are_enemies (0x1401d60bc)
        return (world.is_valid_team(narrow[0]) && world.is_valid_team(narrow[1]) &&
                world.teams[static_cast<std::size_t>(narrow[0])]
                        .relations[static_cast<std::size_t>(narrow[1])] < 0)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1799: // get_scene_boundaries (0x1401d681b)
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            auto& pmin = ctx.positions[static_cast<std::size_t>(narrow[0])];
            auto& pmax = ctx.positions[static_cast<std::size_t>(narrow[1])];
            pmin = core::WseTransform::identity();
            pmin.o = {world.scene_bound_min.x, world.scene_bound_min.y, 0.0f, 1.0f};
            pmax = core::WseTransform::identity();
            pmax.o = {world.scene_bound_max.x, world.scene_bound_max.y, 0.0f, 1.0f};
        }
        return NativeDispatch::handled;
    case 1800: // scene_prop_enable_after_time (0x1401d68eb)
        if (world.is_valid_mission_object(narrow[0])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])].enable_timer =
                static_cast<float>(wide[1]) / 100.0f;
        }
        return NativeDispatch::handled;
    case 1802: // agent_clear_relations_with_agents (0x1401d6971)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            ag.has_relations = false;
            ag.relations.clear();
        }
        return NativeDispatch::handled;
    case 1814: // scene_prop_set_hit_points (0x1401d6dca)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            const float hp = static_cast<float>(wide[1]);
            obj.max_hit_points = hp;
            obj.hit_points = hp;
        }
        return NativeDispatch::handled;
    case 1850: // prop_instance_get_position (0x1401d7a13)
        if (wse_register_ok(narrow[0]) && world.is_valid_mission_object(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.mission_objects[static_cast<std::size_t>(narrow[1])].position;
        }
        return NativeDispatch::handled;
    case 1851: // prop_instance_get_starting_position (0x1401d7bf7)
        if (wse_register_ok(narrow[0]) && world.is_valid_mission_object(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.mission_objects[static_cast<std::size_t>(narrow[1])]
                    .starting_position;
        }
        return NativeDispatch::handled;
    case 1852: // prop_instance_get_scale (0x1401d7cfc)
        if (wse_register_ok(narrow[0]) && world.is_valid_mission_object(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].o =
                world.mission_objects[static_cast<std::size_t>(narrow[1])].scale;
        }
        return NativeDispatch::handled;
    case 1853: // prop_instance_get_scene_prop_kind (0x1401d7d8d)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])]
                        .scene_prop_no),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1854: // prop_instance_set_scale (0x1401d7dd4)
        if (world.is_valid_mission_object(narrow[0])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])].scale = {
                static_cast<float>(wide[1]) / fp,
                static_cast<float>(wide[2]) / fp,
                static_cast<float>(wide[3]) / fp,
                1.0f};
        }
        return NativeDispatch::handled;
    case 1855: // prop_instance_set_position (0x1401d7e6c)
        if (world.is_valid_mission_object(narrow[0]) && wse_register_ok(narrow[1])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])].position =
                ctx.positions[static_cast<std::size_t>(narrow[1])];
        }
        return NativeDispatch::handled;
    case 1860: // prop_instance_animate_to_position (0x1401d7f29)
        if (world.is_valid_mission_object(narrow[0]) && wse_register_ok(narrow[1])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.target_position = ctx.positions[static_cast<std::size_t>(narrow[1])];
            obj.animate_duration = static_cast<float>(wide[2]) / 100.0f;
            obj.animating = true;
        }
        return NativeDispatch::handled;
    case 1861: // prop_instance_stop_animating (0x1401d8024)
        if (world.is_valid_mission_object(narrow[0])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])].animating = false;
        }
        return NativeDispatch::handled;
    case 1862: // prop_instance_is_animating (0x1401d809f)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                world.mission_objects[static_cast<std::size_t>(narrow[1])].animating ? 1
                                                                                    : 0,
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1863: // prop_instance_get_animation_target_position (0x1401d80ea)
        if (wse_register_ok(narrow[0]) && world.is_valid_mission_object(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.mission_objects[static_cast<std::size_t>(narrow[1])]
                    .target_position;
        }
        return NativeDispatch::handled;
    case 1864: // prop_instance_enable_physics (0x1401d8213)
        if (world.is_valid_mission_object(narrow[0])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])].physics_enabled =
                (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1910: // jump_to_scene (0x1401d9569)
        if (world.is_valid_site(narrow[0])) {
            world.jump_scene_no = narrow[0];
            world.jump_entry_no = (statement.numOperands >= 2) ? narrow[1] : 0;
        }
        return NativeDispatch::handled;
    case 1912: // set_jump_entry (0x1401d95cd)
        world.jump_entry_no = narrow[0];
        return NativeDispatch::handled;
    case 1930: // add_reinforcements_to_entry (0x1401d961c)
        game::mission_add_reinforcements(world, narrow[0], narrow[1]);
        return NativeDispatch::handled;
    case 1935: // mission_enable_talk (0x1401d9633)
        world.talk_enabled = true;
        return NativeDispatch::handled;
    case 1936: // mission_disable_talk (0x1401d964d)
        world.talk_enabled = false;
        return NativeDispatch::handled;
    case 1940: // mission_tpl_entry_set_override_flags (0x1401d9667)
        if (world.is_valid_mission_template(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kMaxEntryPoints) {
            world.mission_templates[static_cast<std::size_t>(narrow[0])]
                .entry_overrides[static_cast<std::size_t>(narrow[1])]
                .flags = static_cast<std::uint32_t>(narrow[2]);
        }
        return NativeDispatch::handled;
    case 1941: // mission_tpl_entry_clear_override_items (0x1401d96c1)
        if (world.is_valid_mission_template(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kMaxEntryPoints) {
            world.mission_templates[static_cast<std::size_t>(narrow[0])]
                .entry_overrides[static_cast<std::size_t>(narrow[1])]
                .override_items.clear();
        }
        return NativeDispatch::handled;
    case 1942: // mission_tpl_entry_add_override_item (0x1401d9718)
        if (world.is_valid_mission_template(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kMaxEntryPoints && world.is_valid_item(narrow[2])) {
            world.mission_templates[static_cast<std::size_t>(narrow[0])]
                .entry_overrides[static_cast<std::size_t>(narrow[1])]
                .override_items.push_back(narrow[2]);
        }
        return NativeDispatch::handled;
    case 1943: // mission_tpl_are_all_agents_spawned (0x1401d9796)
        if (world.is_valid_mission_template(world.current_mission_template_no)) {
            return world
                       .mission_templates[static_cast<std::size_t>(
                           world.current_mission_template_no)]
                       .all_agents_spawned
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::handled;
    case 1969: // particle_system_burst (0x1401d9e78)
        if (world.is_valid_particle_system(narrow[0]) && wse_register_ok(narrow[1])) {
            const std::int32_t strength = (narrow[2] != 0) ? narrow[2] : 100;
            game::mission_burst_particle_system(
                world, narrow[0], ctx.positions[static_cast<std::size_t>(narrow[1])], strength,
                true);
        }
        return NativeDispatch::handled;
    case 1970: // set_spawn_position (0x1401d9f5d)
        if (wse_register_ok(narrow[0])) {
            world.spawn_position = ctx.positions[static_cast<std::size_t>(narrow[0])];
        }
        return NativeDispatch::handled;
    case 1972: // spawn_agent (0x1401da0ec)
        ctx.registers[0] = native_campaign_spawn_agent(narrow[0], world);
        return NativeDispatch::handled;
    case 1977: // agent_get_item_cur_ammo (0x1401da28d)
        if (world.is_valid_agent(narrow[1]) && narrow[2] >= 0 && narrow[2] < 4) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.agents[static_cast<std::size_t>(narrow[1])]
                        .item_ammo[static_cast<std::size_t>(narrow[2])]),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2002: // mission_get_time_speed (0x1401db048)
        store_lvalue(dst, static_cast<std::int64_t>(f2i_round(world.mission_time_speed * fp)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2003: // mission_set_time_speed (0x1401db08a)
        if (wide[0] > 0) {
            world.mission_time_speed = static_cast<float>(wide[0]) / fp;
            world.mission_time_speed_duration = 0.0f;
        }
        return NativeDispatch::handled;
    case 2004: // mission_time_speed_move_to_value (0x1401db0e1)
        if (wide[0] > 0 && wide[1] > 0) {
            world.mission_time_speed_target = static_cast<float>(wide[0]) / fp;
            world.mission_time_speed_duration = static_cast<float>(wide[1]) / 1000.0f;
        }
        return NativeDispatch::handled;
    case 2006: // mission_set_duel_mode (0x1401db180)
        world.mission_duel_mode = (narrow[0] != 0);
        return NativeDispatch::handled;
    case 2065: // agent_get_damage_modifier (0x1401dc487)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(world.agents[static_cast<std::size_t>(narrow[1])]
                                       .damage_modifier *
                                   100.0f),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2066: // agent_get_accuracy_modifier (0x1401dc4df)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(world.agents[static_cast<std::size_t>(narrow[1])]
                                       .accuracy_modifier *
                                   100.0f),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2067: // agent_get_speed_modifier (0x1401dc537)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(world.agents[static_cast<std::size_t>(narrow[1])]
                                       .speed_modifier *
                                   100.0f),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2068: // agent_get_reload_speed_modifier (0x1401dc58f)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(world.agents[static_cast<std::size_t>(narrow[1])]
                                       .reload_speed_modifier *
                                   100.0f),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2069: // agent_get_use_speed_modifier (0x1401dc5e7)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(dst,
                         f2i_round(world.agents[static_cast<std::size_t>(narrow[1])]
                                       .use_speed_modifier *
                                   100.0f),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2090: // agent_set_max_hit_points (0x1401dca46)
        if (world.is_valid_agent(narrow[0])) {
            auto& ag = world.agents[static_cast<std::size_t>(narrow[0])];
            float new_max = static_cast<float>(wide[1]);
            if (!(statement.numOperands >= 3 && narrow[2] == 1)) {
                new_max = (new_max * ag.max_hit_points) / 100.0f;
            }
            ag.max_hit_points = std::max(0.0f, new_max);
            ag.hit_points = std::min(ag.hit_points, ag.max_hit_points);
        }
        return NativeDispatch::handled;
    case 2091: // agent_set_damage_modifier (0x1401dcaa3)
        native_campaign_set_agent_mod(narrow[0], wide[1], false,
                                      &game::WseAgent::damage_modifier, world);
        return NativeDispatch::handled;
    case 2092: // agent_set_accuracy_modifier (0x1401dcaf1)
        native_campaign_set_agent_mod(narrow[0], wide[1], true,
                                      &game::WseAgent::accuracy_modifier, world);
        return NativeDispatch::handled;
    case 2093: // agent_set_speed_modifier (0x1401dcbae)
        native_campaign_set_agent_mod(narrow[0], wide[1], true,
                                      &game::WseAgent::speed_modifier, world);
        return NativeDispatch::handled;
    case 2094: // agent_set_reload_speed_modifier (0x1401dcc86)
        native_campaign_set_agent_mod(narrow[0], wide[1], true,
                                      &game::WseAgent::reload_speed_modifier, world);
        return NativeDispatch::handled;
    case 2095: // agent_set_use_speed_modifier (0x1401dcd5b)
        native_campaign_set_agent_mod(narrow[0], wide[1], true,
                                      &game::WseAgent::use_speed_modifier, world);
        return NativeDispatch::handled;
    case 2096: // agent_set_visibility (0x1401dce30)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].visible = (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 2097: // agent_get_crouch_mode (0x1401dce5c)
        if (world.is_valid_agent(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.agents[static_cast<std::size_t>(narrow[1])].crouch_mode),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2098: // agent_set_crouch_mode (0x1401dcea3)
        if (world.is_valid_agent(narrow[0])) {
            world.agents[static_cast<std::size_t>(narrow[0])].crouch_mode =
                std::clamp(narrow[1], 0, 1);
        }
        return NativeDispatch::handled;
    case 2099: // agent_set_ranged_damage_modifier (0x1401dcf04)
        native_campaign_set_agent_mod(narrow[0], wide[1], false,
                                      &game::WseAgent::ranged_damage_modifier, world);
        return NativeDispatch::handled;
    case 2154: // store_num_free_stacks (0x1401dd77a)
        store_lvalue(dst, 10, ctx, locals);
        return NativeDispatch::handled;
    case 2165: { // store_item_kind_count (0x1401dd9cb)
        const std::int32_t troop_no =
            (statement.numOperands >= 3) ? narrow[2] : world.player_troop_no;
        if (world.is_valid_troop(troop_no)) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    game::troop_count_items(world, troop_no, narrow[1], false)),
                ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case 2190: // store_relation (0x1401ddd61)
        if (world.is_valid_faction(narrow[1]) && world.is_valid_faction(narrow[2])) {
            const float rel = world.factions[static_cast<std::size_t>(narrow[1])]
                                  .get_relation(static_cast<std::size_t>(narrow[2]));
            store_lvalue(dst, static_cast<std::int64_t>(std::lround(rel * 100.0f)), ctx,
                         locals);
        }
        return NativeDispatch::handled;
    case 2202: // store_encountered_party (0x1401ddea3)
        store_lvalue(dst, static_cast<std::int64_t>(world.encountered_party_no), ctx, locals);
        return NativeDispatch::handled;
    case 2203: // store_encountered_party2 (0x1401dded1)
        store_lvalue(dst, static_cast<std::int64_t>(world.encountered_party_2_no), ctx,
                     locals);
        return NativeDispatch::handled;
    case 2205: // set_encountered_party (0x1401ddf46)
        if (narrow[0] == -1 || world.is_valid_party(narrow[0])) {
            world.encountered_party_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 2221: // set_zoom_amount (0x1401ddfdb)
        world.zoom_amount = static_cast<float>(wide[0]) / fp;
        return NativeDispatch::handled;
    case 2222: // is_zoom_disabled (0x1401de006)
        return world.zoom_disabled ? NativeDispatch::handled
                                   : NativeDispatch::condition_false;
    case 2230: // store_item_value (0x1401de012)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.items[static_cast<std::size_t>(narrow[1])].value),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2250: // store_random_quest_in_range (0x1401de120)
        store_lvalue(dst, static_cast<std::int64_t>(native_campaign_random_quest(narrow[1], narrow[2], ctx)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2251: // store_random_troop_to_raise (0x1401de155)
        store_lvalue(dst, static_cast<std::int64_t>(native_campaign_random_troop_in_chain(narrow[1], false, ctx)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2252: // store_random_troop_to_capture (0x1401de18a)
        store_lvalue(dst, static_cast<std::int64_t>(native_campaign_random_troop_in_chain(narrow[1], true, ctx)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2254: // store_random_party_in_range (0x1401de1bf)
        store_lvalue(dst, static_cast<std::int64_t>(native_campaign_random_party(narrow[1], narrow[2], ctx)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2255: { // store01_random_parties_in_range (0x1401de1f4)
        const std::int32_t first = native_campaign_random_party(narrow[0], narrow[1], ctx);
        std::int32_t second = first;
        for (std::int32_t i = 0; i < 64 && second == first; ++i) {
            second = native_campaign_random_party(narrow[0], narrow[1], ctx);
        }
        ctx.registers[0] = first;
        ctx.registers[1] = second;
        return NativeDispatch::handled;
    }
    case 2257: // store_random_horse (0x1401de24c)
        store_lvalue(dst, static_cast<std::int64_t>(native_campaign_random_item({1}, ctx)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2258: // store_random_equipment (0x1401de292)
        store_lvalue(dst, static_cast<std::int64_t>(native_campaign_random_item({2, 3, 4}, ctx)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2259: // store_random_armor (0x1401de2d4)
        store_lvalue(dst, static_cast<std::int64_t>(native_campaign_random_item({12, 13, 14, 15}, ctx)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2261: // store_quest_number (0x1401de318)
        if (world.is_valid_quest(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.quests[static_cast<std::size_t>(narrow[1])].number),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2262: // store_quest_item (0x1401de35f)
        if (world.is_valid_quest(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.quests[static_cast<std::size_t>(narrow[1])].item_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2263: // store_quest_troop (0x1401de3a6)
        if (world.is_valid_quest(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.quests[static_cast<std::size_t>(narrow[1])].troop_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2272: // store_current_day (0x1401de46e)
        store_lvalue(dst, static_cast<std::int64_t>(world.current_day), ctx, locals);
        return NativeDispatch::handled;
    case 2273: // is_currently_night (0x1401de49c)
        return world.currently_night ? NativeDispatch::handled
                                     : NativeDispatch::condition_false;
    case 2290: // get_party_ai_behavior (0x1401de544)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.parties[static_cast<std::size_t>(narrow[1])].default_behavior),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2291: // get_party_ai_object (0x1401de58b)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.parties[static_cast<std::size_t>(narrow[1])]
                        .default_behavior_object),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2292: // party_get_ai_target_position (0x1401de5d2)
        if (wse_register_ok(narrow[0]) && world.is_valid_party(narrow[1])) {
            auto& pos = ctx.positions[static_cast<std::size_t>(narrow[0])];
            pos = core::WseTransform::identity();
            const auto& p = world.parties[static_cast<std::size_t>(narrow[1])]
                                .default_behavior_position;
            pos.o = {p.x, p.y, 0.0f, 1.0f};
        }
        return NativeDispatch::handled;
    case 2293: // get_party_ai_current_behavior (0x1401de661)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.parties[static_cast<std::size_t>(narrow[1])].behavior),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2294: // get_party_ai_current_object (0x1401de6a8)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.parties[static_cast<std::size_t>(narrow[1])].behavior_object),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2310: { // store_num_parties_of_template (0x1401de7d0)
        std::int32_t count = 0;
        if (world.is_valid_party_template(narrow[1])) {
            for (const auto& party : world.parties) {
                if (party.active && party.template_no == narrow[1]) {
                    ++count;
                }
            }
        }
        store_lvalue(dst, static_cast<std::int64_t>(count), ctx, locals);
        return NativeDispatch::handled;
    }
    case 2311: { // store_random_party_of_template (0x1401de889)
        if (!world.is_valid_party_template(narrow[1])) {
            return NativeDispatch::condition_false;
        }
        std::int32_t count = 0;
        for (const auto& party : world.parties) {
            if (party.active && party.template_no == narrow[1]) {
                ++count;
            }
        }
        if (count == 0) {
            return NativeDispatch::condition_false;
        }
        const std::int32_t target_idx = (count > 1) ? native_rgl_rand(ctx, count) : 0;
        std::int32_t seen = 0;
        for (std::size_t i = 0; i < world.parties.size(); ++i) {
            if (world.parties[i].active && world.parties[i].template_no == narrow[1]) {
                if (seen == target_idx) {
                    store_lvalue(dst, static_cast<std::int64_t>(i), ctx, locals);
                    return NativeDispatch::handled;
                }
                ++seen;
            }
        }
        return NativeDispatch::condition_false;
    }
    case 2332: // str_store_agent_name (0x1401ded62)
        if (wse_register_ok(narrow[0]) && world.is_valid_agent(narrow[1])) {
            const std::int32_t tno =
                world.agents[static_cast<std::size_t>(narrow[1])].troop_no;
            if (world.is_valid_troop(tno)) {
                ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                    wse_utf8_to_utf16(world.troops[static_cast<std::size_t>(tno)].name);
            }
        }
        return NativeDispatch::handled;
    case 2346: // str_store_class_name (0x1401df268)
        if (wse_register_ok(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kNumDivisions) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(world.class_names[static_cast<std::size_t>(narrow[1])]);
        }
        return NativeDispatch::handled;
    case 2365: // store_mission_timer_a_msec (0x1401df3e0)
        store_lvalue(dst,
                     static_cast<std::int64_t>(
                         world.mission_timers[0].elapsed_seconds(ctx.timersTotal) *
                         1000.0f),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2366: // store_mission_timer_b_msec (0x1401df420)
        store_lvalue(dst,
                     static_cast<std::int64_t>(
                         world.mission_timers[1].elapsed_seconds(ctx.timersTotal) *
                         1000.0f),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2367: // store_mission_timer_c_msec (0x1401df460)
        store_lvalue(dst,
                     static_cast<std::int64_t>(
                         world.mission_timers[2].elapsed_seconds(ctx.timersTotal) *
                         1000.0f),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2375: // reset_mission_timer_a (0x1401df548)
        native_campaign_reset_timer(0, ctx);
        return NativeDispatch::handled;
    case 2376: // reset_mission_timer_b (0x1401df560)
        native_campaign_reset_timer(1, ctx);
        return NativeDispatch::handled;
    case 2377: // reset_mission_timer_c (0x1401df578)
        native_campaign_reset_timer(2, ctx);
        return NativeDispatch::handled;
    case 2379: // set_cheer_at_no_enemy (0x1401df590)
        world.cheer_at_no_enemy = (narrow[0] != 0);
        return NativeDispatch::handled;

    // Item Kind Getters (2700..2724)
    case 2700: // item_get_weight (0x1401e0be3)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.items[static_cast<std::size_t>(narrow[1])].weight * fp),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2701: // item_get_value (0x1401e0c41)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].value, ctx,
                         locals);
        }
        return NativeDispatch::handled;
    case 2702: // item_get_difficulty (0x1401e0c8c)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].difficulty,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2703: // item_get_head_armor (0x1401e0cd7)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].head_armor,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2704: // item_get_body_armor (0x1401e0d22)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].body_armor,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2705: // item_get_leg_armor (0x1401e0d6d)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].leg_armor,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2706: // item_get_hit_points (0x1401e0db8)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].hit_points,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2707: // item_get_weapon_length (0x1401e0e03)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst,
                         world.items[static_cast<std::size_t>(narrow[1])].weapon_length,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2708: // item_get_speed_rating (0x1401e0e4e)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst,
                         world.items[static_cast<std::size_t>(narrow[1])].speed_rating,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2709: // item_get_missile_speed (0x1401e0e99)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst,
                         world.items[static_cast<std::size_t>(narrow[1])].missile_speed,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2710: // item_get_max_ammo (0x1401e0ee4)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].max_ammo,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2711: // item_get_accuracy (0x1401e0f2f)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst,
                         world.items[static_cast<std::size_t>(narrow[1])].get_accuracy(),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2712: // item_get_shield_height (0x1401e0f7a)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.items[static_cast<std::size_t>(narrow[1])].get_shield_height() *
                    fp),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2713: // item_get_horse_scale (0x1401e0fd6)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.items[static_cast<std::size_t>(narrow[1])].get_horse_scale() *
                    fp),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2714: // item_get_horse_speed (0x1401e1032)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst, world.items[static_cast<std::size_t>(narrow[1])].get_horse_speed(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2715: // item_get_horse_maneuver (0x1401e107d)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst, world.items[static_cast<std::size_t>(narrow[1])].get_horse_maneuver(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2716: // item_get_food_quality (0x1401e10c8)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst, world.items[static_cast<std::size_t>(narrow[1])].get_food_quality(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2717: // item_get_abundance (0x1401e1113)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, world.items[static_cast<std::size_t>(narrow[1])].abundance,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2718: // item_get_thrust_damage (0x1401e115e)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst,
                world.items[static_cast<std::size_t>(narrow[1])].get_thrust_damage(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2719: // item_get_thrust_damage_type (0x1401e11ab)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst,
                world.items[static_cast<std::size_t>(narrow[1])].get_thrust_damage_type(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2720: // item_get_swing_damage (0x1401e11f8)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst, world.items[static_cast<std::size_t>(narrow[1])].get_swing_damage(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2721: // item_get_swing_damage_type (0x1401e1245)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst,
                world.items[static_cast<std::size_t>(narrow[1])].get_swing_damage_type(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2722: // item_get_horse_charge_damage (0x1401e1292)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(
                dst, world.items[static_cast<std::size_t>(narrow[1])].get_horse_charge(),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2723: // item_has_property (0x1401e12dd)
        return (world.is_valid_item(narrow[0]) &&
                (world.items[static_cast<std::size_t>(narrow[0])].properties &
                 static_cast<std::uint64_t>(wide[1])) != 0ULL)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 2724: // item_has_capability (0x1401e131a)
        return (world.is_valid_item(narrow[0]) &&
                (world.items[static_cast<std::size_t>(narrow[0])].capabilities &
                 static_cast<std::uint64_t>(wide[1])) != 0ULL)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 2750: // str_store_troop_face_keys (0x1401e15b8)
        if (wse_register_ok(narrow[0]) && world.is_valid_troop(narrow[1])) {
            const std::int32_t slot =
                (statement.numOperands >= 3 && narrow[2] != 0) ? 1 : 0;
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(
                    world.troops[static_cast<std::size_t>(narrow[1])]
                        .face_keys_to_string(slot));
        }
        return NativeDispatch::handled;
    case 2751: { // troop_set_face_keys (0x1401e1697)
        if (world.is_valid_troop(narrow[0])) {
            const std::uint8_t tag1 = (tags.size() > 1) ? tags[1] : 0U;
            const std::string hex =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
            if (hex.size() == 64U) {
                const std::int32_t slot =
                    (statement.numOperands >= 3 && narrow[2] != 0) ? 1 : 0;
                world.troops[static_cast<std::size_t>(narrow[0])].face_keys_from_string(
                    slot, hex);
            }
        }
        return NativeDispatch::handled;
    }

    default:
        break;
    }
    return dispatch_native_world_statement(opcode, wide, narrow, tags, statement, ctx, locals);
}

}  // namespace wse::vm

#include "wse/vm/native_world_ops.h"
