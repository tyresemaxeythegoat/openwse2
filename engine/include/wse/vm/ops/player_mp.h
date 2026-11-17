// Milestone 6 multiplayer, server, player, and string-MP opcode handlers
// (53 WSE opcodes: 2900..2909, 3400..3416, 3500..3520, 4225, 4227, 4228, 4230, 4237).

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "wse/vm/wse_state.h"

namespace wse::vm {

// Mirrors mbBasicGame::isValidPassword: length < 50 and chars in [0-9A-Za-z_].
[[nodiscard]] inline bool wse_m6_is_valid_password(std::string_view s) noexcept {
    if (s.size() >= 0x32U) {
        return false;
    }
    for (const char ch : s) {
        const auto uc = static_cast<unsigned char>(ch);
        if (uc >= static_cast<unsigned char>('a') &&
            uc <= static_cast<unsigned char>('z')) {
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

[[nodiscard]] inline WseDispatch wse_mp_store_utf8(WseStringBanks& strings,
                                                   std::int32_t sreg,
                                                   std::string_view utf8) {
    return wse_store_string(strings, sreg, wse_utf8_to_utf16(utf8))
               ? WseDispatch::handled_no_store
               : WseDispatch::ignored;
}

[[nodiscard]] inline bool wse_mp_can_mutate_net_game(
    const wse::game::WseWorldState& world) noexcept {
    return world.is_multiplayer &&
           (world.is_server || !world.m6.server.switching_to_campaign);
}

inline void wse_mp_stamp_net_header(wse::game::WseNetBuffer& buf,
                                    std::int32_t msg_type) {
    const std::int32_t orig_size = buf.size;
    const std::int32_t orig_cursor = buf.cursor;
    buf.cursor = 4;
    buf.put_uint16(static_cast<std::uint16_t>(orig_size), 12);
    buf.put_uint16(static_cast<std::uint16_t>(msg_type), 8);
    buf.size = orig_size;
    buf.cursor = orig_cursor;
}

[[nodiscard]] inline WseDispatch execute_wse_player_mp(
    std::uint32_t opcode,
    const WseOperands& operands,
    WseEffect& effect) {
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    auto* world = operands.state.world;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::player_set_skin: {
        // 2900 (0x1401e4f8e): sets player skin in multiplayer when compat broken.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !world->is_valid_player(narrow[0]) || narrow[1] < 0 ||
            narrow[1] >= world->num_skins ||
            !wse_mp_can_mutate_net_game(*world)) {
            return WseDispatch::ignored;
        }
        world->players[static_cast<std::size_t>(narrow[0])].profile_gender =
            narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::player_stop_controlling_agent: {
        // 2901 (0x1401e5079): returns controlled agent to AI control.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !world->is_valid_player(narrow[0]) ||
            !wse_mp_can_mutate_net_game(*world)) {
            return WseDispatch::ignored;
        }
        auto& pl = world->players[static_cast<std::size_t>(narrow[0])];
        const std::int32_t agent_no = pl.agent_no;
        if (world->is_valid_agent(agent_no)) {
            auto& ag = world->agents[static_cast<std::size_t>(agent_no)];
            ag.player_no = -1;
            ag.is_non_player = true;
            ag.m5.action_channels[0].progress = 0.0f;
            ag.m5.action_channels[1].progress = 0.0f;
        }
        pl.agent_no = -1;
        world->m6.player(narrow[0]).controlling_stopped = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::player_set_banner_id: {
        // 2902 (0x1401e51f2)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_player(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->players[static_cast<std::size_t>(narrow[0])].banner_no =
            narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::player_set_username: {
        // 2903 (0x1401e5226): parses string operand 1 into player name.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string parsed;
        if (world == nullptr || !world->is_valid_player(narrow[0]) ||
            !wse_resolve_parsed_utf8_operand(1, operands, parsed)) {
            return WseDispatch::ignored;
        }
        world->players[static_cast<std::size_t>(narrow[0])].name =
            std::move(parsed);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::player_temp_ban: {
        // 2904 (0x1401e52e1): bans non-admin non-host player for wide[1] > 0.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_player(narrow[0])) {
            return WseDispatch::ignored;
        }
        auto& pl = world->players[static_cast<std::size_t>(narrow[0])];
        if (pl.privilege > 2 || narrow[0] == 0 || wide[1] < 1 ||
            !wse_mp_can_mutate_net_game(*world)) {
            return WseDispatch::ignored;
        }
        wse::game::WseBanRecord rec{};
        rec.player_no = narrow[0];
        rec.unique_id = pl.unique_id;
        rec.name = pl.name;
        rec.permanent = false;
        rec.banner_player_no = narrow[0];
        world->mp.banned_players.push_back(std::move(rec));
        (void)world->disconnect_player(narrow[0], true);
        world->m6.player(narrow[0]).temp_banned_minutes =
            static_cast<std::int32_t>(
                std::min<std::int64_t>(wide[1],
                                       std::numeric_limits<std::int32_t>::max()));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::player_get_wse2_version: {
        // 2905 (0x1401e537f)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_player(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto it = world->m6.players.find(narrow[1]);
        effect.value = (it != world->m6.players.end()) ? it->second.wse2_version : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::player_get_party_id: {
        // 2906 (0x1401e53b7)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_player(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto it = world->m6.players.find(narrow[1]);
        effect.value = (it != world->m6.players.end()) ? it->second.party_no : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::player_set_party_id: {
        // 2907 (0x1401e53ef -> 0x1400e7330)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_player(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::int32_t player_no = narrow[0];
        const std::int32_t new_party = narrow[1];
        auto& plex = world->m6.player(player_no);
        if (plex.party_no == new_party) {
            return WseDispatch::handled_no_store;
        }
        if (new_party != -1 && !world->is_valid_party(new_party)) {
            return WseDispatch::ignored;
        }
        if (world->is_valid_party(plex.party_no)) {
            auto& old_p = world->parties[static_cast<std::size_t>(plex.party_no)];
            old_p.player_no = -1;
            old_p.controller = 1;
        }
        plex.party_no = new_party;
        if (new_party != -1) {
            auto& new_p = world->parties[static_cast<std::size_t>(new_party)];
            new_p.player_no = player_no;
            new_p.controller = 0;
        }
        if (player_no == world->my_player_no) {
            world->misc.camp_bandits_party_no = std::max<std::int32_t>(0, new_party);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::player_get_anticheat_stat: {
        // 2908 (0x1401e5442 -> WSEAntiCheat::getStat)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_player(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value = world->m6.anticheat.get_stat_scaled(
            narrow[1], narrow[2], operands.state.fixed_point_multiplier);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::player_anticheat_report: {
        // 2909 (0x1401e546b -> WSEAntiCheat::report)
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_player(narrow[0]) ||
            !wse_mp_can_mutate_net_game(*world)) {
            return WseDispatch::ignored;
        }
        const auto& pl = world->players[static_cast<std::size_t>(narrow[0])];
        (void)world->m6.anticheat.report(narrow[0], 100, wide[1], 0, true,
                                         pl.privilege);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_send_chat_message_to_player: {
        // 3400 (0x1401e96e3)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string text;
        if (world == nullptr || !world->is_server ||
            !world->is_valid_player(narrow[0]) ||
            !world->is_valid_player(narrow[1]) ||
            !wse_resolve_parsed_utf8_operand(2, operands, text)) {
            return WseDispatch::ignored;
        }
        world->m6.chat_messages.emplace_back(narrow[0], text);
        if (narrow[0] == world->my_player_no) {
            world->displayed_messages.push_back(
                {wse_utf8_to_utf16(text), 0xffffffffU});
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_send_composite_message_to_player: {
        // 3401 (0x1401e97f6)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !world->is_valid_player(narrow[0]) || narrow[1] < 0 ||
            narrow[1] >= 256 || !wse_register_ok(narrow[2])) {
            return WseDispatch::ignored;
        }
        auto& buf = world->m6.net_buffers[narrow[2]];
        wse_mp_stamp_net_header(buf, narrow[1]);
        if (!world->is_server) {
            return WseDispatch::ignored;
        }
        world->m6.sent_composite_messages.push_back(
            {narrow[0], narrow[1], buf});
        if (narrow[0] == world->my_player_no) {
            world->m6.active_net_buffer = buf;
            world->m6.active_net_buffer.cursor =
                std::min<std::int32_t>(24, world->m6.active_net_buffer.size);
            world->m6.has_trigger_net_buffer = true;
            world->m6.last_received_message_type = narrow[1];
            world->m6.last_received_sender_player = narrow[0];
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_send_composite_message_to_server: {
        // 3402 (0x1401e998b)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            narrow[0] < 0 || narrow[0] >= 256 || !wse_register_ok(narrow[1])) {
            return WseDispatch::ignored;
        }
        auto& buf = world->m6.net_buffers[narrow[1]];
        wse_mp_stamp_net_header(buf, narrow[0]);
        if (world->is_server) {
            world->m6.sent_composite_messages.push_back({-1, narrow[0], buf});
            world->m6.active_net_buffer = buf;
            world->m6.active_net_buffer.cursor =
                std::min<std::int32_t>(24, world->m6.active_net_buffer.size);
            world->m6.has_trigger_net_buffer = true;
            world->m6.last_received_message_type = narrow[0];
            world->m6.last_received_sender_player = 0;
        } else if (world->is_multiplayer) {
            world->m6.sent_composite_messages.push_back({-1, narrow[0], buf});
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_get_cur_profile: {
        // 3403 (0x1401e9ac9)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = world->mp.current_profile_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::multiplayer_get_num_profiles: {
        // 3404 (0x1401e9ada)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = static_cast<std::int64_t>(world->mp.profiles.size());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::multiplayer_message_init: {
        // 3405 (0x1401e9aeb): clears buffer and reserves 24-bit header (4+12+8).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        auto& buf = world->m6.net_buffers[narrow[0]];
        buf.clear();
        buf.put_uint16(0, 4);
        buf.put_uint16(0, 12);
        buf.put_uint16(0, 8);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_message_put_string: {
        // 3406 (0x1401e9b6c)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string text;
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !wse_register_ok(narrow[0]) ||
            !wse_resolve_parsed_utf8_operand(1, operands, text)) {
            return WseDispatch::ignored;
        }
        world->m6.net_buffers[narrow[0]].put_string(text, 300);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_message_put_int: {
        // 3407 (0x1401e9c04)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::int32_t bits = (narrow.size() < 3) ? 32 : narrow[2];
        if (bits < 1 || bits > 32) {
            return WseDispatch::ignored;
        }
        world->m6.net_buffers[narrow[0]].put_int32(narrow[1], bits, 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_message_put_position:
    case WseOpcode::multiplayer_message_put_coordinate: {
        // 3408 / 3409 (0x1401e9c74 / 0x1401e9d34)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            static_cast<std::size_t>(narrow[1]) >=
                operands.state.positions.size()) {
            return WseDispatch::ignored;
        }
        const bool compressed = (narrow.size() >= 3 && narrow[2] != 0);
        const auto& pos =
            operands.state.positions[static_cast<std::size_t>(narrow[1])];
        auto& buf = world->m6.net_buffers[narrow[0]];
        if (opcode ==
            static_cast<std::uint32_t>(
                WseOpcode::multiplayer_message_put_position)) {
            buf.put_transform(pos, compressed);
        } else {
            buf.put_vector4(pos.o, compressed);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_cur_message_get_string: {
        // 3410 (0x1401e9dd8)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !world->m6.has_trigger_net_buffer || !wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        auto& buf = world->m6.active_net_buffer;
        const std::string out =
            (buf.cursor + 16 <= buf.size) ? buf.take_string(300) : std::string{};
        return wse_mp_store_utf8(strings, narrow[0], out);
    }
    case WseOpcode::multiplayer_cur_message_get_int: {
        // 3411 (0x1401e9e57)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility) {
            return WseDispatch::ignored;
        }
        std::int32_t bits = (narrow.size() < 2) ? 32 : narrow[1];
        if (bits == 0) {
            bits = 32;
        }
        if (world->m6.has_trigger_net_buffer && bits >= 1 && bits <= 32) {
            effect.value = world->m6.active_net_buffer.take_int32(bits, 0);
        } else {
            effect.value = 0;
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::multiplayer_cur_message_get_position:
    case WseOpcode::multiplayer_cur_message_get_coordinate: {
        // 3412 / 3413 (0x1401e9eae / 0x1401ea020)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !world->m6.has_trigger_net_buffer || !wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >=
                operands.state.positions.size()) {
            return WseDispatch::ignored;
        }
        const bool compressed = (narrow.size() >= 2 && narrow[1] != 0);
        auto& dst =
            operands.state.positions[static_cast<std::size_t>(narrow[0])];
        if (opcode ==
            static_cast<std::uint32_t>(
                WseOpcode::multiplayer_cur_message_get_position)) {
            world->m6.active_net_buffer.take_transform(dst, compressed);
        } else {
            dst.o = world->m6.active_net_buffer.take_vector4(compressed);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_cur_profile_get_skin: {
        // 3414 (0x1401ea0b2)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr ||
            !world->is_valid_profile(world->mp.current_profile_no)) {
            return WseDispatch::ignored;
        }
        const auto it = world->m6.profiles.find(world->mp.current_profile_no);
        effect.value =
            (it != world->m6.profiles.end()) ? it->second.gender_skin : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::multiplayer_connect_to_server: {
        // 3415 (0x1401ea0c7): raw getString on address & password.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string addr;
        std::string pwd;
        if (world == nullptr || !wse_resolve_utf8_operand(0, operands, addr) ||
            !wse_resolve_utf8_operand(1, operands, pwd)) {
            return WseDispatch::ignored;
        }
        world->m6.server.stored_connect_password = std::move(pwd);
        world->m6.server.stored_connect_address = std::move(addr);
        if (world->m6.server.stored_connect_address.empty()) {
            return WseDispatch::handled_no_store;
        }
        world->m6.server.switching_module = true;
        world->m6.server.switching_to_campaign =
            (narrow.size() >= 3 && narrow[2] != 0);
        world->m6.server.connect_to_server = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::multiplayer_is_campaign: {
        // 3416 (0x1401ea170 -> mbBasicGame::isMultiplayerCampaign)
        return (world != nullptr && world->m6.server.switching_to_campaign)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::server_set_password_admin:
    case WseOpcode::server_set_password_private: {
        // 3500 / 3501 (0x1401ea184 / 0x1401ea217)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string pwd;
        if (world == nullptr || !world->is_multiplayer ||
            !wse_resolve_utf8_operand(0, operands, pwd)) {
            return WseDispatch::ignored;
        }
        if (wse_m6_is_valid_password(pwd)) {
            if (opcode ==
                static_cast<std::uint32_t>(WseOpcode::server_set_password_admin)) {
                world->m6.server.admin_password = std::move(pwd);
            } else {
                world->m6.server.private_password = std::move(pwd);
            }
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::server_map_rotation_get_count:
    case WseOpcode::server_map_rotation_get_index:
    case WseOpcode::server_map_rotation_get_map: {
        // 3502 / 3503 / 3505 (0x1401e812f): retail always stores -1.
        const std::size_t min_ops =
            (opcode ==
             static_cast<std::uint32_t>(WseOpcode::server_map_rotation_get_map))
                ? 2U
                : 1U;
        if (narrow.size() < min_ops) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::server_map_rotation_set_index:
    case WseOpcode::server_map_rotation_add_map:
    case WseOpcode::server_map_rotation_remove_map:
    case WseOpcode::server_set_password_rcon:
    case WseOpcode::execute_server_console_command: {
        // 3504 / 3506 / 3507 / 3513 / 3514 (0x1401f49c1): immediate true no-op.
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::server_get_horse_friendly_fire: {
        // 3508 (0x1401ea2aa)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = (world != nullptr &&
                        world->m6.server.break_warband_compatibility &&
                        world->m6.server.horse_friendly_fire)
                           ? 1
                           : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::server_set_horse_friendly_fire: {
        // 3509 (0x1401ea2e2)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !world->is_server) {
            return WseDispatch::ignored;
        }
        const bool enabled = (narrow[0] != 0);
        world->m6.server.horse_friendly_fire = enabled;
        if (operands.state.options != nullptr) {
            operands.state.options->horse_friendly_fire = enabled;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::server_get_show_crosshair: {
        // 3510 (0x1401ea336)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = (world != nullptr &&
                        world->m6.server.break_warband_compatibility &&
                        world->m6.server.show_crosshair)
                           ? 1
                           : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::server_set_show_crosshair: {
        // 3511 (0x1401ea361)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.server.break_warband_compatibility ||
            !world->is_server) {
            return WseDispatch::ignored;
        }
        const bool enabled = (narrow[0] != 0);
        world->m6.server.show_crosshair = enabled;
        if (operands.state.options != nullptr) {
            operands.state.options->show_crosshair = enabled;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_server_option_at_connect: {
        // 3512 (0x1401ea3b5)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_multiplayer || world->is_server) {
            return WseDispatch::ignored;
        }
        const std::int32_t idx = (narrow.size() >= 2) ? narrow[1] : 0;
        std::int64_t found = -1;
        for (const auto& entry : world->m6.server.server_list) {
            if (entry.ip == world->m6.server.active_server_ip &&
                entry.port == world->m6.server.active_server_port) {
                if (idx >= 0 &&
                    static_cast<std::size_t>(idx) <
                        entry.module_settings.size()) {
                    found = entry.module_settings[static_cast<std::size_t>(idx)];
                }
                break;
            }
        }
        effect.value = found;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::add_anonymous_player: {
        // 3515 (0x1401ea530)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string name;
        if (world == nullptr || !world->is_server ||
            !wse_resolve_parsed_utf8_operand(1, operands, name)) {
            return WseDispatch::ignored;
        }
        world->m6.server
            .anonymous_player_names[static_cast<std::uint32_t>(narrow[0])] =
            std::move(name);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::remove_anonymous_player: {
        // 3516 (0x1401ea5c3)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_server) {
            return WseDispatch::ignored;
        }
        world->m6.server.anonymous_player_names.erase(
            static_cast<std::uint32_t>(narrow[0]));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::clear_anonymous_players: {
        // 3517 (0x1401ea5fa)
        if (world == nullptr || !world->is_server) {
            return WseDispatch::ignored;
        }
        world->m6.server.anonymous_player_names.clear();
        world->m6.server.anonymous_players.clear();
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::server_set_ghost_mode_advanced: {
        // 3518 (0x1401ea61f)
        if (narrow.size() < 5) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_server) {
            return WseDispatch::ignored;
        }
        world->m6.server.ghost_advanced_enabled = (narrow[0] != 0);
        world->m6.server.ghost_disable_free_cam = (narrow[1] != 0);
        world->m6.server.ghost_free_roam = (narrow[1] == 0);
        world->m6.server.ghost_lock_to_team = (narrow[2] != 0);
        world->m6.server.ghost_lock_to_agent = (narrow[3] != 0);
        world->m6.server.ghost_only_players = (narrow[4] != 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::server_set_anticheat_option: {
        // 3519 (0x1401ea669)
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_server) {
            return WseDispatch::ignored;
        }
        world->m6.anticheat.set_option(narrow[0], wide[1], narrow[1],
                                       operands.state.fixed_point_multiplier);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::server_get_anticheat_option: {
        // 3520 (0x1401ea68e)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr ||
            static_cast<std::uint32_t>(narrow[1]) >= 87U) {
            return WseDispatch::ignored;
        }
        effect.value = world->m6.anticheat.get_option(
            narrow[1], operands.state.fixed_point_multiplier);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_store_multiplayer_profile_name: {
        // 4225 (0x1401eef9c)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || world == nullptr ||
            !world->is_valid_profile(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto it = world->m6.profiles.find(narrow[1]);
        const std::string_view name =
            (it != world->m6.profiles.end()) ? std::string_view{it->second.name}
                                             : std::string_view{};
        return wse_mp_store_utf8(strings, narrow[0], name);
    }
    case WseOpcode::str_store_server_password_admin:
    case WseOpcode::str_store_server_password_private: {
        // 4227 / 4228 (0x1401ef126 / 0x1401ef160)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || world == nullptr) {
            return WseDispatch::ignored;
        }
        const std::string_view pwd =
            (opcode ==
             static_cast<std::uint32_t>(
                 WseOpcode::str_store_server_password_admin))
                ? std::string_view{world->m6.server.admin_password}
                : std::string_view{world->m6.server.private_password};
        return wse_mp_store_utf8(strings, narrow[0], pwd);
    }
    case WseOpcode::str_store_player_ip: {
        // 4230 (0x1401ef213)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || world == nullptr ||
            !world->is_valid_player(narrow[1]) || !world->is_server) {
            return WseDispatch::ignored;
        }
        const auto it = world->m6.players.find(narrow[1]);
        const std::string_view ip =
            (it != world->m6.players.end())
                ? std::string_view{it->second.ip_address}
                : std::string_view{};
        return wse_mp_store_utf8(strings, narrow[0], ip);
    }
    case WseOpcode::str_store_multiplayer_profile_face_keys: {
        // 4237 (0x1401ef676)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || world == nullptr ||
            !world->is_valid_profile(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& fk =
            world->mp.profiles[static_cast<std::size_t>(narrow[1])].face_keys;
        return wse_mp_store_utf8(strings, narrow[0], fk.to_string());
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
