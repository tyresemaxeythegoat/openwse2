// Module-system script records and the `scripts.txt` reader.
//
// Evidence (Ghidra 12.1.4 pseudocode plus PDB TPI):
//   mbScript::readStream         0x14020f6a0  (?readStream@mbScript@@QEAAXAEAVrglStream@@H@Z)
//   mbScriptManager::readStream  0x14020f7c0
//   mbScriptManager::mapScripts  0x14020f9a0
//   mbScript layout              176 bytes, PDB type 0x1f7b8
//     +0x00 m_id rglString, +0x48 m_interval float, +0x50 m_statementBlock
//   mbScriptManager layout       272 bytes, PDB type 0xaeea
//     +0x00 m_scripts mbScript*, +0x08 m_numScripts int32,
//     +0x0c m_mappedScripts int32[65]
//   Globals: g_scriptManager 0x14069fac0, count 0x14069fac8,
//     mapped table 0x14069facc, name table 0x140615ad0.
//   Format strings: "Script [%d] %s" 0x14055c680,
//     "Unable to map script %s" 0x14055c690.
//
// `mbScriptManager::readStream` discards two strings (the retail header is
// `scriptsfile version`), discards one int32 version, reads the int32 script
// count from stream slot +0x80, replaces the script array, calls
// `mbScript::readStream(&m_scripts[i], stream, i)` for every entry, then calls
// `mapScripts()`.
//
// `mbScript::readStream` reads the id string (slot +0x90), the interval float
// (slot +0x58), the statement block, and then replaces the block id with
// `rglFormat("Script [%d] %s", index, m_id)`.
//
// `mapScripts` walks the 65-entry name table in order. For each name it scans
// the scripts in file order and stores the first index whose `m_id` compares
// exactly equal; a name with no match stores -1 and logs
// `rglInfo("Unable to map script %s", name)`. Duplicate ids therefore resolve
// to the first script in file order.
//
// Safety deviation: retail multiplies the unchecked count by 176 and reads
// that many entries. This reader rejects negative or oversized counts and
// truncated records; valid module files parse identically.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/io/text_stream.h"
#include "wse/vm/statement.h"

namespace wse::game {

inline constexpr std::size_t kMaxScripts = 1u << 20;
inline constexpr std::size_t kMappedScriptCount = 65;

// Static 65-entry name table at 0x140615ad0, in retail order.
inline constexpr std::array<std::string_view, kMappedScriptCount> kMappedScriptNames = {
    "game_start",
    "game_quick_start",
    "game_set_multiplayer_mission_end",
    "game_get_console_command",
    "game_enable_cheat_menu",
    "game_event_party_encounter",
    "game_event_simulate_battle",
    "game_get_item_buy_price_factor",
    "game_get_item_sell_price_factor",
    "game_event_buy_item",
    "game_event_sell_item",
    "game_get_troop_wage",
    "game_get_total_wage",
    "game_get_join_cost",
    "game_get_prisoner_price",
    "game_check_prisoner_can_be_sold",
    "game_event_detect_party",
    "game_event_undetect_party",
    "game_get_statistics_line",
    "game_get_date_text",
    "game_get_money_text",
    "game_get_party_companion_limit",
    "game_get_party_prisoner_limit",
    "game_get_item_extra_text",
    "game_on_disembark",
    "game_context_menu_get_buttons",
    "game_event_context_menu_button_clicked",
    "game_get_skill_modifier_for_troop",
    "game_reset_player_party_name",
    "game_check_party_sees_party",
    "game_get_party_speed_multiplier",
    "game_event_battle_end",
    "game_get_troop_note",
    "game_get_center_note",
    "game_get_faction_note",
    "game_get_quest_note",
    "game_get_info_page_note",
    "game_get_scene_name",
    "game_get_mission_template_name",
    "game_receive_network_message",
    "game_get_multiplayer_server_option_for_mission_template",
    "game_multiplayer_server_option_for_mission_template_to_string",
    "game_get_use_string",
    "game_get_upgrade_xp",
    "game_get_upgrade_cost",
    "game_get_morale_of_troops_from_faction",
    "game_multiplayer_get_game_type_mission_template",
    "game_get_multiplayer_game_type_enum",
    "game_receive_url_response",
    "game_get_cheat_mode",
    "game_multiplayer_event_duel_offered",
    "game_missile_launch",
    "game_missile_dives_into_water",
    "game_troop_upgrades_button_clicked",
    "game_character_screen_requested",
    "wse_chat_message_received",
    "wse_console_command_received",
    "wse_game_saved",
    "wse_multiplayer_message_received",
    "wse_get_agent_scale",
    "wse_window_opened",
    "wse_savegame_loaded",
    "wse_get_server_info",
    "wse_initial_window_start",
    "wse_camp_button_clicked",
};

struct Script {
    std::string id;
    float interval = 0.0F;
    vm::StatementBlock block;
};

struct ScriptManager {
    std::vector<Script> scripts;
    std::array<std::int32_t, kMappedScriptCount> mappedScripts;

    ScriptManager() { mappedScripts.fill(-1); }
};

// Reports every name in the static table that has no script. Retail logs
// through rglInfo; tests and the Android layer supply their own sink.
struct ScriptMapCallbacks {
    void (*reportMissing)(void* context, std::string_view name) = nullptr;
    void* context = nullptr;
};

// One <id> <interval> <statement block> record, in that order.
[[nodiscard]] inline vm::ReadError read_script(io::TokenReader& reader,
                                               Script& script,
                                               std::int32_t index) {
    if (!reader.read_string(script.id)) {
        return vm::ReadError::malformed_string;
    }
    if (!reader.read_f32(script.interval)) {
        return vm::ReadError::malformed_number;
    }

    const vm::ReadError error = vm::read_statement_block(reader, script.block);
    if (error != vm::ReadError::none) {
        return error;
    }

    std::string id;
    id.reserve(script.id.size() + 24);
    id.append("Script [");
    id.append(std::to_string(index));
    id.append("] ");
    id.append(script.id);
    script.block.id = std::move(id);
    return vm::ReadError::none;
}

// Scans the 65-entry name table and stores the first exact id match, or -1.
inline void map_scripts(ScriptManager& manager,
                        const ScriptMapCallbacks& callbacks = {}) {
    for (std::size_t slot = 0; slot < kMappedScriptCount; ++slot) {
        const std::string_view name = kMappedScriptNames[slot];
        std::int32_t found = -1;
        for (std::size_t index = 0; index < manager.scripts.size(); ++index) {
            if (manager.scripts[index].id == name) {
                found = static_cast<std::int32_t>(index);
                break;
            }
        }
        if (found < 0 && callbacks.reportMissing != nullptr) {
            callbacks.reportMissing(callbacks.context, name);
        }
        manager.mappedScripts[slot] = found;
    }
}

// Reads the two header strings, the discarded version int32, the script count,
// every script record, and finally maps the static names.
[[nodiscard]] inline vm::ReadError read_script_manager(io::TokenReader& reader,
                                                       ScriptManager& manager,
                                                       const ScriptMapCallbacks& callbacks = {}) {
    std::string header;
    if (!reader.read_string(header)) {
        return vm::ReadError::malformed_string;
    }
    if (!reader.read_string(header)) {
        return vm::ReadError::malformed_string;
    }

    std::int32_t version = 0;
    if (!reader.read_i32(version)) {
        return vm::ReadError::malformed_number;
    }

    std::int32_t count = 0;
    if (!reader.read_i32(count)) {
        return vm::ReadError::malformed_number;
    }
    if (count < 0) {
        return vm::ReadError::negative_count;
    }
    if (static_cast<std::uint64_t>(count) > kMaxScripts) {
        return vm::ReadError::script_count_out_of_range;
    }

    // Shortest record: "<id> <interval> <statementCount>" plus a separator.
    manager.scripts.clear();
    manager.scripts.reserve(reader.plausible_count(static_cast<std::size_t>(count), 6));
    for (std::int32_t index = 0; index < count; ++index) {
        Script& script = manager.scripts.emplace_back();
        const vm::ReadError error = read_script(reader, script, index);
        if (error != vm::ReadError::none) {
            return error;
        }
    }

    map_scripts(manager, callbacks);
    return vm::ReadError::none;
}

}  // namespace wse::game
