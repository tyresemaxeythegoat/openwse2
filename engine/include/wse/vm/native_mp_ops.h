// Native multiplayer & player opcode handlers (Milestone 5, 48 opcodes).
// Grounded in official WSE2 v1.1.5.5 PDB/binary decompilation:
//   multiplayer_and_players (48): 388..393, 409, 410, 425, 429, 431, 433..442,
//                                 459, 460, 475, 476, 478..499, 2748

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_scene_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

// Mirrors mbBasicGame::isValidPassword: length < 50 and chars in [0-9A-Za-z_].
[[nodiscard]] inline bool native_m5_is_valid_password(std::string_view s) noexcept {
    if (s.size() >= 0x32U) {
        return false;
    }
    for (const char ch : s) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc >= static_cast<unsigned char>('a') && uc <= static_cast<unsigned char>('z')) {
            continue;
        }
        const int off = static_cast<int>(uc) - 0x30;
        if (off >= 0 && off <= 0x2f &&
            ((0x87fffffe03ffULL >> static_cast<unsigned int>(off)) & 1ULL) != 0ULL) {
            continue;
        }
        return false;
    }
    return true;
}

// Mirrors mbBasicGame::isValidServerName: length < 50 and allowed server name charset.
[[nodiscard]] inline bool native_m5_is_valid_server_name(std::string_view s) noexcept {
    if (s.size() >= 0x32U) {
        return false;
    }
    for (const char ch : s) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc >= static_cast<unsigned char>('a') && uc <= static_cast<unsigned char>('z')) {
            continue;
        }
        if (uc == static_cast<unsigned char>('~')) {
            continue;
        }
        const int off = static_cast<int>(uc) - 0x2a;
        if (off >= 0 && off <= 0x35 &&
            ((0x2bffffff80ffc9ULL >> static_cast<unsigned int>(off)) & 1ULL) != 0ULL) {
            continue;
        }
        return false;
    }
    return true;
}

inline void native_m5_send_to_server(std::int32_t event_type,
                                     std::span<const std::int64_t> ints,
                                     const std::u16string& text,
                                     bool has_text,
                                     VmContext& ctx) {
    auto& world = ctx.world;
    if (world.is_multiplayer && !world.is_server) {
        game::WseNetClientEvent ev{};
        ev.event_type = event_type;
        ev.num_ints = static_cast<std::int32_t>(std::min<std::size_t>(ints.size(), 4U));
        for (std::int32_t i = 0; i < ev.num_ints; ++i) {
            ev.ints[static_cast<std::size_t>(i)] = ints[static_cast<std::size_t>(i)];
        }
        ev.text = text;
        world.m5.client_events.push_back(std::move(ev));
        return;
    }
    if (has_text) {
        ctx.stringRegisters[0] = text;
    }
    if (ctx.scripts != nullptr) {
        std::array<std::int64_t, 6> params{
            static_cast<std::int64_t>(world.my_player_no),
            static_cast<std::int64_t>(event_type),
            (ints.size() > 0) ? ints[0] : 0,
            (ints.size() > 1) ? ints[1] : 0,
            (ints.size() > 2) ? ints[2] : 0,
            (ints.size() > 3) ? ints[3] : 0};
        (void)execute_mapped_script(*ctx.scripts, 0x27U, ctx, params, ctx.currentCaller);
    }
}

// Mirrors mbMission::findSpawnPoint (64 entry points scored by spacing and effectors).
[[nodiscard]] inline std::int32_t native_m5_find_spawn_point(
    std::int32_t team_no,
    std::uint32_t flags,
    VmContext& ctx) noexcept {
    const auto& world = ctx.world;
    std::int32_t alive_humans = 0;
    for (const auto& ag : world.agents) {
        if (ag.active && ag.is_human() && ag.status == 1) {
            ++alive_humans;
        }
    }
    std::int32_t rand_denom = 2;
    if (alive_humans > 7) {
        if (alive_humans < 16) {
            rand_denom = 3;
        } else if (alive_humans < 32) {
            rand_denom = 4;
        } else {
            rand_denom = (alive_humans > 63) ? 6 : 5;
        }
    }

    auto sq_dist = [](const wse::core::WseVec4& a, const wse::core::WseVec4& b) noexcept {
        const float dx = a.x - b.x;
        const float dy = a.y - b.y;
        const float dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };

    float best_score = -1.0e6f;
    std::int32_t best_entry = -1;
    for (std::int32_t pass = 0; pass < 2; ++pass) {
        if (best_score >= 0.2f) {
            return best_entry;
        }
        for (std::int32_t ep = 0; ep < 64; ++ep) {
            if (ep == best_entry) {
                continue;
            }
            if (rand_denom >= 2 && native_rgl_rand(ctx, rand_denom) != 0) {
                continue;
            }
            const auto& pos = world.entry_points[static_cast<std::size_t>(ep)].position.o;

            float min_team_eff_sq = -1.0f;
            float min_other_eff_sq = 250000.0f;
            if (world.fx.spawn_effector_scene_prop_kinds[0] >= 0 ||
                world.fx.spawn_effector_scene_prop_kinds[1] >= 0) {
                for (const std::int32_t mo_no : world.fx.spawn_effector_mission_object_nos) {
                    if (!world.is_valid_mission_object(mo_no)) {
                        continue;
                    }
                    const auto& mo = world.mission_objects[static_cast<std::size_t>(mo_no)];
                    const float d2 = sq_dist(mo.position.o, pos);
                    if (team_no >= 0 && team_no < 2 &&
                        mo.scene_prop_no ==
                            world.fx.spawn_effector_scene_prop_kinds[static_cast<std::size_t>(team_no)]) {
                        if (min_team_eff_sq < 0.0f || d2 < min_team_eff_sq) {
                            min_team_eff_sq = d2;
                        }
                    } else if (d2 < min_other_eff_sq) {
                        min_other_eff_sq = d2;
                    }
                }
            }
            min_other_eff_sq = std::min(min_other_eff_sq, 16900.0f);
            const float eff_len = std::clamp(std::sqrt(std::fabs(min_team_eff_sq)), 40.0f, 170.0f);

            bool crowded = false;
            float enemy_d2 = 250000.0f;
            float enemy_d2_2 = 250000.0f;
            float friend_d2 = 250000.0f;
            float friend_d2_2 = 250000.0f;
            for (const auto& ag : world.agents) {
                if (!ag.active || !ag.is_human() || ag.status != 1) {
                    continue;
                }
                const float d2 = sq_dist(ag.position.o, pos);
                if ((flags & 1U) == 0U && ag.team_no == team_no) {
                    if (d2 < 0.64f) {
                        crowded = true;
                        friend_d2 = 0.0f;
                    } else if (d2 < friend_d2) {
                        friend_d2_2 = friend_d2;
                        friend_d2 = d2;
                    }
                } else {
                    if (d2 < 4.0f) {
                        crowded = true;
                    } else if (d2 < enemy_d2) {
                        enemy_d2_2 = enemy_d2;
                        enemy_d2 = d2;
                    }
                }
            }
            float w_base = crowded ? 0.0f : 1.0f;
            float w_enemy = 1.0f;
            if (enemy_d2 < 9.0f) {
                w_enemy = 0.2f;
            } else if (enemy_d2 < 16.0f) {
                w_enemy = 0.4f;
            } else if (enemy_d2 < 36.0f) {
                w_enemy = 0.6f;
            } else if (enemy_d2 < 64.0f) {
                w_enemy = 0.8f;
            }
            w_base *= w_enemy;
            if (!crowded) {
                const float c_e1 = std::min(enemy_d2, 900.0f);
                const float c_e2 = std::min(enemy_d2_2, 900.0f);
                const float f1 = std::min(1.0f, (std::max(friend_d2, 900.0f) - 900.0f) / 1600.0f);
                const float f2 = std::min(1.0f, (std::max(friend_d2_2, 900.0f) - 900.0f) / 1600.0f);
                const bool half_penalty = ((flags >> 12U) & 1U) != 0U;
                const float s1 = half_penalty ? 0.1f : 0.2f;
                const float s2 = half_penalty ? 0.06f : 0.12f;
                w_base = ((w_base - ((900.0f - c_e1) / 900.0f) * w_enemy * s1) -
                          ((900.0f - c_e2) / 900.0f) * w_enemy * s2) -
                         f1 * w_enemy * s1 - f2 * w_enemy * s2;
            }
            const float jitter =
                static_cast<float>(native_rgl_rand(ctx, 15817)) / 15817.0f * 0.1f;
            const float score = (1.0f - jitter) * w_base *
                                ((1.0f - ((eff_len - 40.0f) / 130.0f) * 0.2f) -
                                 (1.0f - min_other_eff_sq / 16900.0f) * 0.6f);
            if (best_score < score) {
                best_score = score;
                best_entry = ep;
            }
        }
    }
    return best_entry;
}

[[nodiscard]] inline NativeDispatch dispatch_native_mp_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    const auto banks = native_string_banks(ctx);
    auto& world = ctx.world;

    switch (opcode) {
    case 388: // multiplayer_send_message_to_server (0x1401c774c)
        native_m5_send_to_server(narrow[0], {}, {}, false, ctx);
        return NativeDispatch::handled;
    case 389: // multiplayer_send_int_to_server (0x1401c77d9)
        native_m5_send_to_server(narrow[0], wide.subspan(1, 1), {}, false, ctx);
        return NativeDispatch::handled;
    case 390: // multiplayer_send_2_int_to_server (0x1401c7851)
        native_m5_send_to_server(narrow[0], wide.subspan(1, 2), {}, false, ctx);
        return NativeDispatch::handled;
    case 391: // multiplayer_send_3_int_to_server (0x1401c78d4)
        native_m5_send_to_server(narrow[0], wide.subspan(1, 3), {}, false, ctx);
        return NativeDispatch::handled;
    case 392: // multiplayer_send_4_int_to_server (0x1401c7952)
        native_m5_send_to_server(narrow[0], wide.subspan(1, 4), {}, false, ctx);
        return NativeDispatch::handled;
    case 393: { // multiplayer_send_string_to_server (0x1401c79db)
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (wse_string_register_ok(narrow[1], tag1, banks)) {
            const std::u16string parsed = native_resolve_and_parse(narrow[1], tag1, ctx);
            native_m5_send_to_server(narrow[0], {}, parsed, true, ctx);
        }
        return NativeDispatch::handled;
    }
    case 409: // player_spawn_new_agent (0x1401c8316)
        if (world.is_valid_player(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kMaxEntryPoints) {
            auto& pl = world.players[static_cast<std::size_t>(narrow[0])];
            pl.m5.ready_to_spawn = true;
            pl.m5.spawn_entry_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 410: // player_add_spawn_item (0x1401c8365)
        if (world.is_valid_player(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kNumTroopEquipmentSlots && world.is_valid_item(narrow[2])) {
            world.players[static_cast<std::size_t>(narrow[0])]
                .item_ids[static_cast<std::size_t>(narrow[1])] = narrow[2];
        }
        return NativeDispatch::handled;
    case 425: // multiplayer_find_spawn_point (0x1401c8752)
        if (world.is_valid_team(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(native_m5_find_spawn_point(
                    narrow[1], static_cast<std::uint32_t>(narrow[2]), ctx)),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 429: // player_set_is_admin (0x1401c8828)
        if (world.is_valid_player(narrow[0])) {
            auto& pl = world.players[static_cast<std::size_t>(narrow[0])];
            pl.privilege = (wide[1] != 0) ? 3 : 1;
            pl.is_admin = (wide[1] != 0);
        }
        return NativeDispatch::handled;
    case 431: // player_get_score (0x1401c88b9)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].score),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 433: // player_get_kill_count (0x1401c8932)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].kill_count),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 434: // player_set_kill_count (0x1401c897a)
        if (world.is_valid_player(narrow[0])) {
            world.players[static_cast<std::size_t>(narrow[0])].kill_count =
                static_cast<std::int16_t>(narrow[1]);
        }
        return NativeDispatch::handled;
    case 435: // player_get_death_count (0x1401c89ab)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].death_count),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 436: // player_set_death_count (0x1401c89f3)
        if (world.is_valid_player(narrow[0])) {
            world.players[static_cast<std::size_t>(narrow[0])].death_count =
                static_cast<std::int16_t>(narrow[1]);
        }
        return NativeDispatch::handled;
    case 437: // player_get_ping (0x1401c8a24)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].ping),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 438: // player_is_busy_with_menus (0x1401c8a6c)
        return (world.is_valid_player(narrow[0]) &&
                world.players[static_cast<std::size_t>(narrow[0])].m5.is_busy_with_menus)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 439: // player_get_is_muted (0x1401c8a9c)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         world.players[static_cast<std::size_t>(narrow[1])].is_muted ? 1 : 0,
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 440: // player_set_is_muted (0x1401c8ae7)
        if (world.is_valid_player(narrow[0])) {
            auto& pl = world.players[static_cast<std::size_t>(narrow[0])];
            pl.is_muted = (wide[1] != 0);
            if (world.is_server && statement.numOperands >= 3 && wide[2] != 0) {
                pl.m5.is_muted_globally = (wide[1] != 0);
            }
        }
        return NativeDispatch::handled;
    case 441: // player_get_unique_id (0x1401c8b56)
        if (world.is_valid_player(narrow[1])) {
            const std::uint32_t uid =
                static_cast<std::uint32_t>(
                    world.players[static_cast<std::size_t>(narrow[1])].unique_id) &
                0xffffffU;
            store_lvalue(dst, static_cast<std::int64_t>(uid), ctx, locals);
        }
        return NativeDispatch::handled;
    case 442: // player_get_gender (0x1401c8ba0)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].profile_gender),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 459: // player_save_picked_up_items_for_next_spawn (0x1401c8e47)
        if (world.is_valid_player(narrow[0]) && world.is_server) {
            auto& pl = world.players[static_cast<std::size_t>(narrow[0])];
            pl.item_ids.fill(-1);
            pl.picked_up_slots.fill(false);
            if (world.is_valid_troop(pl.troop_no) && world.is_valid_agent(pl.agent_no)) {
                const auto& ag = world.agents[static_cast<std::size_t>(pl.agent_no)];
                for (std::size_t slot = 0; slot < 8U; ++slot) {
                    const std::int32_t kind = ag.equipped_items[slot];
                    if (!world.is_valid_item(kind)) {
                        continue;
                    }
                    const std::int32_t item_type =
                        world.items[static_cast<std::size_t>(kind)].get_type();
                    if (item_type != 5 && item_type != 6 && item_type != 0x12) {
                        pl.item_ids[slot] = kind;
                        pl.picked_up_slots[slot] = true;
                    }
                }
                if (world.is_valid_agent(ag.horse_agent_no)) {
                    const auto& horse = world.agents[static_cast<std::size_t>(ag.horse_agent_no)];
                    if (horse.rider_agent_no < 0 || horse.rider_agent_no == pl.agent_no) {
                        pl.item_ids[8] = horse.horse_item_no;
                        pl.picked_up_slots[8] = true;
                    }
                }
                pl.m5.spawn_items_saved = true;
            }
        }
        return NativeDispatch::handled;
    case 460: // player_get_value_of_original_items (0x1401c8e88)
        if (world.is_valid_player(narrow[1])) {
            const auto& pl = world.players[static_cast<std::size_t>(narrow[1])];
            if (world.is_valid_agent(pl.agent_no) && world.is_server) {
                const auto& ag = world.agents[static_cast<std::size_t>(pl.agent_no)];
                std::int32_t total = 0;
                for (std::size_t slot = 0; slot < 8U; ++slot) {
                    const std::int32_t kind = ag.equipped_items[slot];
                    if (world.is_valid_item(kind)) {
                        total += world.items[static_cast<std::size_t>(kind)].value;
                    }
                }
                if (world.is_valid_item(ag.horse_item_no)) {
                    for (std::size_t idx = 0; idx < world.agents.size(); ++idx) {
                        const auto& other = world.agents[idx];
                        if (other.active && other.status == 1 && other.rider_agent_no == pl.agent_no) {
                            if (static_cast<std::int32_t>(idx) == ag.horse_agent_no ||
                                !world.is_valid_agent(ag.horse_agent_no)) {
                                total += world.items[static_cast<std::size_t>(ag.horse_item_no)].value;
                            }
                            break;
                        }
                    }
                }
                store_lvalue(dst, static_cast<std::int64_t>(total), ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 475: // server_get_renaming_server_allowed (0x1401c921b)
        store_lvalue(dst, world.m5.allow_renaming_server ? 1 : 0, ctx, locals);
        return NativeDispatch::handled;
    case 476: // server_get_changing_game_type_allowed (0x1401c9242)
        store_lvalue(dst, world.m5.allow_changing_game_type ? 1 : 0, ctx, locals);
        return NativeDispatch::handled;
    case 478: // server_get_combat_speed (0x1401c9269)
        if (world.is_multiplayer) {
            store_lvalue(dst, static_cast<std::int64_t>(world.m5.server_combat_speed), ctx, locals);
        }
        return NativeDispatch::handled;
    case 479: // server_set_combat_speed (0x1401c92a4)
        if (world.is_multiplayer && narrow[0] >= 0 && narrow[0] < 5) {
            world.m5.server_combat_speed = narrow[0];
        }
        return NativeDispatch::handled;
    case 480: // server_get_friendly_fire (0x1401c9308)
        if (world.is_multiplayer) {
            store_lvalue(dst, static_cast<std::int64_t>(world.m5.server_friendly_fire), ctx, locals);
        }
        return NativeDispatch::handled;
    case 481: // server_set_friendly_fire (0x1401c9343)
        if (world.is_multiplayer && narrow[0] >= 0 && narrow[0] < 2) {
            world.m5.server_friendly_fire = narrow[0];
        }
        return NativeDispatch::handled;
    case 482: // server_get_control_block_dir (0x1401c937d)
        if (world.is_multiplayer) {
            store_lvalue(
                dst, static_cast<std::int64_t>(world.m5.server_control_block_dir), ctx, locals);
        }
        return NativeDispatch::handled;
    case 483: // server_set_control_block_dir (0x1401c93b8)
        if (world.is_multiplayer && narrow[0] >= 0 && narrow[0] < 2) {
            world.m5.server_control_block_dir = narrow[0];
        }
        return NativeDispatch::handled;
    case 484: { // server_set_password (0x1401c941c)
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (world.is_multiplayer && wse_string_register_ok(narrow[0], tag0, banks)) {
            const std::string utf8 =
                wse_utf16_to_utf8(wse_resolve_string(narrow[0], tag0, banks));
            if (native_m5_is_valid_password(utf8)) {
                world.mp.server_password = utf8;
            }
        }
        return NativeDispatch::handled;
    }
    case 485: // server_get_add_to_game_servers_list (0x1401c94bb)
        if (world.is_multiplayer) {
            store_lvalue(
                dst, static_cast<std::int64_t>(world.m5.server_add_to_servers_list), ctx, locals);
        }
        return NativeDispatch::handled;
    case 486: // server_set_add_to_game_servers_list (0x1401c94f6)
        if (world.is_multiplayer) {
            world.m5.server_add_to_servers_list = (wide[0] != 0) ? 1 : 0;
        }
        return NativeDispatch::handled;
    case 487: // server_get_ghost_mode (0x1401c9523)
        if (world.is_multiplayer) {
            store_lvalue(dst, static_cast<std::int64_t>(world.m5.server_ghost_mode), ctx, locals);
        }
        return NativeDispatch::handled;
    case 488: // server_set_ghost_mode (0x1401c955e)
        if (world.is_multiplayer && narrow[0] >= 0 && narrow[0] < 4) {
            world.m5.server_ghost_mode = narrow[0];
        }
        return NativeDispatch::handled;
    case 489: { // server_set_name (0x1401c959a)
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (world.is_multiplayer && wse_string_register_ok(narrow[0], tag0, banks) &&
            world.m5.allow_renaming_server) {
            const std::string utf8 =
                wse_utf16_to_utf8(wse_resolve_string(narrow[0], tag0, banks));
            if (native_m5_is_valid_server_name(utf8)) {
                world.mp.server_name = utf8;
            } else {
                const std::int32_t suffix = 1 + native_rgl_rand(ctx, 9999);
                world.mp.server_name = "Server" + std::to_string(suffix);
            }
        }
        return NativeDispatch::handled;
    }
    case 490: // server_get_max_num_players (0x1401c9654)
        if (world.is_multiplayer) {
            store_lvalue(dst, static_cast<std::int64_t>(world.max_players), ctx, locals);
        }
        return NativeDispatch::handled;
    case 491: // server_set_max_num_players (0x1401c968f)
        if (world.is_multiplayer) {
            const std::int32_t max_pub = narrow[0];
            const std::int32_t max_priv =
                (statement.numOperands >= 2) ? narrow[1] : narrow[0];
            if (max_pub >= 2 && max_pub <= game::kMaxPlayers &&
                max_priv >= max_pub && max_priv <= game::kMaxPlayers) {
                world.max_players = max_pub;
                world.m5.max_private_players = max_priv;
            }
        }
        return NativeDispatch::handled;
    case 492: { // server_set_welcome_message (0x1401c9706)
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (world.is_multiplayer && wse_string_register_ok(narrow[0], tag0, banks)) {
            world.mp.welcome_message =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx));
        }
        return NativeDispatch::handled;
    }
    case 493: // server_get_melee_friendly_fire (0x1401c9786)
        if (world.is_multiplayer) {
            store_lvalue(
                dst, static_cast<std::int64_t>(world.m5.server_melee_friendly_fire), ctx, locals);
        }
        return NativeDispatch::handled;
    case 494: // server_set_melee_friendly_fire (0x1401c97c1)
        if (world.is_multiplayer && narrow[0] >= 0 && narrow[0] < 2) {
            world.m5.server_melee_friendly_fire = narrow[0];
        }
        return NativeDispatch::handled;
    case 495: // server_get_friendly_fire_damage_self_ratio (0x1401c97fb)
        if (world.is_multiplayer) {
            store_lvalue(
                dst, static_cast<std::int64_t>(world.m5.server_ff_self_ratio), ctx, locals);
        }
        return NativeDispatch::handled;
    case 496: // server_set_friendly_fire_damage_self_ratio (0x1401c9836)
        if (world.is_multiplayer && narrow[0] >= 0 && narrow[0] <= 100) {
            world.m5.server_ff_self_ratio = narrow[0];
        }
        return NativeDispatch::handled;
    case 497: // server_get_friendly_fire_damage_friend_ratio (0x1401c9870)
        if (world.is_multiplayer) {
            store_lvalue(
                dst, static_cast<std::int64_t>(world.m5.server_ff_friend_ratio), ctx, locals);
        }
        return NativeDispatch::handled;
    case 498: // server_set_friendly_fire_damage_friend_ratio (0x1401c98ab)
        if (world.is_multiplayer && narrow[0] >= 0 && narrow[0] <= 100) {
            world.m5.server_ff_friend_ratio = narrow[0];
        }
        return NativeDispatch::handled;
    case 499: // server_get_anti_cheat (0x1401c98e5)
        if (world.is_multiplayer) {
            store_lvalue(dst, 0, ctx, locals);
        }
        return NativeDispatch::handled;
    case 2748: { // player_set_face_keys (0x1401e1454)
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (world.is_valid_player(narrow[0]) && wse_string_register_ok(narrow[1], tag1, banks)) {
            const std::string utf8 =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
            if (utf8.size() == 64U) {
                (void)world.players[static_cast<std::size_t>(narrow[0])].face_keys.from_string(
                    utf8);
            }
        }
        return NativeDispatch::handled;
    }

    default:
        break;
    }
    return dispatch_native_scene_statement(opcode, wide, narrow, tags, statement, ctx, locals);
}

}  // namespace wse::vm

#include "wse/vm/native_scene_ops.h"
