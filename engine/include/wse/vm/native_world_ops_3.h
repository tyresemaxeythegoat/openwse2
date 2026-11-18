// Extended native Warband opcode handlers (other_native tier, 80 opcodes).
// Grounded in official WSE2 v1.1.5.6 PDB/binary decompilation:
//   input/options/profile (16): 78, 260..271, 350, 351, 380
//   spawn effectors & net bans (6): 426, 427, 465..468
//   mp mission/audio/pos/ui (13): 470, 616, 702, 715, 960, 970..972, 980, 985,
//                                 988, 990, 991
//   dialog/notes/quests/encounters (19): 1025, 1090..1092, 1109, 1111, 1112,
//                                        1115, 1118, 1130, 1285, 1290, 1291,
//                                        1300, 1301, 1303..1305, 1307
//   mission/tableau/talk/menu (16): 1805, 1829, 1891, 1920, 1950, 1955, 1982,
//                                   1983, 1986, 2020..2022, 2031, 2032, 2061, 2198
//   lighting/skybox/postfx (10): 2386, 2387, 2389..2396

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_agent_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

inline constexpr std::size_t kMaxNativeMissileSpawns = 1024;
inline constexpr std::int32_t kMaxNativeItemModifier = 43;  // 0x2b

/// Safely converts a finite float into a 64-bit signed integer with saturation.
[[nodiscard]] inline std::int64_t native_world_3_f2i(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }
    return game::native_f2i_saturate(value);
}

/// Safely rounds a finite float into a 64-bit signed integer.
[[nodiscard]] inline std::int64_t native_world_3_round(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0;
    }
    return wse_round64(value);
}

/// Stores a damage multiplier option either as quantized 0/1/2 or integer percent.
inline void native_world_get_damage_option(float value,
                                           std::int32_t mode,
                                           std::uint64_t dst,
                                           VmContext& ctx,
                                           std::span<std::int64_t> locals) {
    if (mode == 0) {
        const std::int64_t tier =
            (value < 0.75f) ? ((value >= 0.5f) ? 1 : 0) : 2;
        store_lvalue(dst, tier, ctx, locals);
        return;
    }
    store_lvalue(dst, native_world_3_f2i(value * 100.0f), ctx, locals);
}

/// Sets a damage multiplier option using either tiered mode (0) or percentage mode.
inline void native_world_set_damage_option(std::int32_t raw_val,
                                           std::int32_t mode,
                                           float& target) noexcept {
    if (mode == 0) {
        if (raw_val >= 0 && raw_val < 3) {
            target = (raw_val < 1) ? 0.25f : static_cast<float>(raw_val) * 0.5f;
        }
        return;
    }
    if (raw_val >= 0 && raw_val < 101) {
        target = static_cast<float>(raw_val) / 100.0f;
    }
}

/// Updates an integer option when within [0, upper_exclusive).
inline void native_world_set_bounded_option(std::int32_t value,
                                            std::int32_t upper_exclusive,
                                            std::int32_t& target) noexcept {
    if (value >= 0 && value < upper_exclusive) {
        target = value;
    }
}

/// Populates the UI item tooltip state for show_item_details variants.
inline void native_world_set_item_tooltip(game::WseWorldState& world,
                                          const VmContext& ctx,
                                          std::int32_t item_no,
                                          std::int32_t modifier,
                                          std::int32_t reg_idx,
                                          std::int32_t mode) noexcept {
    if (!world.is_valid_item(item_no) || !wse_register_ok(reg_idx) || mode < 0) {
        return;
    }
    const auto& pos = ctx.positions[static_cast<std::size_t>(reg_idx)].o;
    const std::int32_t clamped_mod =
        (static_cast<std::uint32_t>(modifier) <
         static_cast<std::uint32_t>(kMaxNativeItemModifier))
            ? modifier
            : 0;
    world.ui.item_tooltip = game::WseItemTooltipRequest{
        true, item_no, clamped_mod, wse::core::WseVec2{pos.x, pos.y}, mode};
}

/// Appends a ban entry for an active non-admin player when running on the server.
inline void native_world_push_ban_record(game::WseWorldState& world,
                                         std::int32_t player_no,
                                         std::int32_t unique_id,
                                         std::string name,
                                         bool permanent,
                                         std::int32_t banner_player_no) {
    game::WseBanRecord record{};
    record.player_no = player_no;
    record.unique_id = unique_id;
    record.name = std::move(name);
    record.permanent = permanent;
    record.banner_player_no = banner_player_no;
    game::push_bounded(world.mp.banned_players, std::move(record),
                       game::kMaxBans);
}

/// Resolves the root party along attached_to_party_no links with cycle protection.
[[nodiscard]] inline std::int32_t native_world_root_party_no(
    const game::WseWorldState& world,
    std::int32_t party_no) noexcept {
    if (!world.is_valid_party(party_no)) {
        return party_no;
    }
    std::int32_t cur = party_no;
    const std::size_t max_hops = world.parties.size();
    for (std::size_t step = 0; step < max_hops; ++step) {
        const std::int32_t parent =
            world.parties[static_cast<std::size_t>(cur)].attached_to_party_no;
        if (parent < 0 || parent == cur || !world.is_valid_party(parent)) {
            break;
        }
        cur = parent;
    }
    return cur;
}

/// Resolves an optional translation key fallback and evaluates VM string tokens.
[[nodiscard]] inline std::string native_world_parse_quest_text(
    const game::WseWorldState& world,
    std::string_view key,
    std::string_view fallback,
    VmContext& ctx) {
    std::string_view raw = fallback;
    if (!key.empty()) {
        const auto it = world.translations.find(std::string(key));
        if (it != world.translations.end()) {
            raw = it->second;
        }
    }
    const auto banks = native_string_banks(ctx);
    const std::u16string parsed =
        wse_parse_string(wse_utf8_to_utf16(raw), ctx.registers, banks);
    return wse_utf16_to_utf8(parsed);
}

/// Computes current Warband difficulty percentage matching mbGame::getDifficulty.
[[nodiscard]] inline std::int32_t native_world_current_difficulty(
    const VmContext& ctx) noexcept {
    const float save_f =
        (ctx.world.misc.save_mode_flags == 0) ? 1.0f : 0.75f;
    const float dmg_self = std::isfinite(ctx.options.damage_to_player)
                               ? std::max(ctx.options.damage_to_player, 0.01f)
                               : 0.01f;
    const float dmg_self_f =
        1.0f + 0.15f * (std::log(dmg_self) / std::log(2.0f));
    const float battle_size = std::isfinite(ctx.options.battle_size)
                                  ? ctx.options.battle_size
                                  : 1.0f;
    const float bs_f = 1.0f + (battle_size - 0.5f) * 0.15f;
    const float cai_f =
        1.0f - static_cast<float>(ctx.options.combat_ai) * 0.09f;
    const float cmp_f =
        1.0f - static_cast<float>(ctx.options.campaign_ai) * 0.15f;
    const float dmg_fr = std::isfinite(ctx.options.damage_to_friends)
                             ? ctx.options.damage_to_friends
                             : 1.0f;
    const float dmg_fr_f = 1.0f - (1.0f - dmg_fr) * 0.8f;
    constexpr float kLanceFactor = 0.95f;
    const float speed_f =
        0.9f + static_cast<float>(ctx.options.combat_speed) * 0.05f;
    const float raw = save_f * dmg_self_f * bs_f * cai_f * cmp_f * dmg_fr_f *
                      kLanceFactor * speed_f * 100.01000213623047f;
    if (!std::isfinite(raw) || raw <= 0.0f) {
        return 0;
    }
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        game::native_f2i_saturate(raw), 0,
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
}

/// Reads fixed-point RGB components from operands 0..2 into an RGBA vector's XYZ.
inline void native_world_set_fp_rgb(std::span<const std::int64_t> wide,
                                    float fp,
                                    wse::core::WseVec4& dst) noexcept {
    dst.x = static_cast<float>(wide[0]) / fp;
    dst.y = static_cast<float>(wide[1]) / fp;
    dst.z = static_cast<float>(wide[2]) / fp;
}

/// Copies an RGB light color into pos[reg_idx].o.xyz if permitted.
inline void native_world_store_light_rgb(std::int32_t reg_idx,
                                         bool active,
                                         const wse::core::WseVec4& color,
                                         bool zero_if_inactive,
                                         VmContext& ctx) noexcept {
    if (!wse_register_ok(reg_idx)) {
        return;
    }
    auto& out = ctx.positions[static_cast<std::size_t>(reg_idx)].o;
    if (!active) {
        if (zero_if_inactive) {
            out = wse::core::WseVec4{0.0f, 0.0f, 0.0f, 0.0f};
        }
        return;
    }
    out.x = color.x;
    out.y = color.y;
    out.z = color.z;
}

[[nodiscard]] inline NativeDispatch dispatch_native_world_3_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    const std::uint8_t tag0 = tags.size() > 0 ? tags[0] : 0U;
    const std::uint8_t tag1 = tags.size() > 1 ? tags[1] : 0U;
    const std::uint8_t tag2 = tags.size() > 2 ? tags[2] : 0U;
    auto& world = ctx.world;
    const float fp =
        ctx.fixedPointMultiplier != 0
            ? static_cast<float>(ctx.fixedPointMultiplier)
            : 1.0f;

    switch (opcode) {
    // -----------------------------------------------------------------
    // input, options, profiles, and HTTP (16 opcodes)
    // -----------------------------------------------------------------
    case 78: // clear_omitted_keys (0x1401c6b62)
        world.input.omitted_keys.fill(false);
        return NativeDispatch::handled;
    case 260: // options_get_damage_to_player (0x1401c71f9)
        native_world_get_damage_option(ctx.options.damage_to_player, narrow[1],
                                       dst, ctx, locals);
        return NativeDispatch::handled;
    case 261: // options_set_damage_to_player (0x1401c7275)
        native_world_set_damage_option(narrow[0], narrow[1],
                                       ctx.options.damage_to_player);
        return NativeDispatch::handled;
    case 262: // options_get_damage_to_friends (0x1401c7300)
        native_world_get_damage_option(ctx.options.damage_to_friends, narrow[1],
                                       dst, ctx, locals);
        return NativeDispatch::handled;
    case 263: // options_set_damage_to_friends (0x1401c737c)
        native_world_set_damage_option(narrow[0], narrow[1],
                                       ctx.options.damage_to_friends);
        return NativeDispatch::handled;
    case 264: // options_get_combat_ai (0x1401c7407)
        store_lvalue(dst, static_cast<std::int64_t>(ctx.options.combat_ai), ctx,
                     locals);
        return NativeDispatch::handled;
    case 265: // options_set_combat_ai (0x1401c742e)
        native_world_set_bounded_option(narrow[0], 3, ctx.options.combat_ai);
        return NativeDispatch::handled;
    case 266: // options_get_campaign_ai (0x1401c7453)
        store_lvalue(dst, static_cast<std::int64_t>(ctx.options.campaign_ai),
                     ctx, locals);
        return NativeDispatch::handled;
    case 267: // options_set_campaign_ai (0x1401c747a)
        native_world_set_bounded_option(narrow[0], 3, ctx.options.campaign_ai);
        return NativeDispatch::handled;
    case 268: // options_get_combat_speed (0x1401c749f)
        store_lvalue(dst, static_cast<std::int64_t>(ctx.options.combat_speed),
                     ctx, locals);
        return NativeDispatch::handled;
    case 269: // options_set_combat_speed (0x1401c74c6)
        native_world_set_bounded_option(narrow[0], 5, ctx.options.combat_speed);
        return NativeDispatch::handled;
    case 270: // options_get_battle_size (0x1401c74eb)
        store_lvalue(dst,
                     native_world_3_round(ctx.options.battle_size * 1000.0f),
                     ctx, locals);
        return NativeDispatch::handled;
    case 271: // options_set_battle_size (0x1401c7520)
        if (narrow[0] >= 0 && narrow[0] < 10001) {
            ctx.options.battle_size =
                static_cast<float>(wide[0]) / 1000.0f;
        }
        return NativeDispatch::handled;
    case 350: // profile_get_banner_id (0x1401c755a)
        if (world.is_valid_profile(world.mp.current_profile_no)) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mp.profiles[static_cast<std::size_t>(
                                          world.mp.current_profile_no)]
                        .banner_no),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 351: // profile_set_banner_id (0x1401c75ae)
        if (world.is_valid_profile(world.mp.current_profile_no)) {
            world.mp.profiles[static_cast<std::size_t>(
                                  world.mp.current_profile_no)]
                .banner_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 380: { // send_message_to_url (0x1401c7657)
        const auto banks = native_string_banks(ctx);
        if (wse_string_register_ok(narrow[0], tag0, banks)) {
            const bool encode = (wide[1] != 0);
            const std::u16string parsed = wse_parse_string(
                wse_resolve_string(narrow[0], tag0, banks), ctx.registers,
                banks, encode);
            game::WseUrlRequestRecord req{};
            req.url = wse_utf16_to_utf8(parsed);
            req.raw = !encode;
            game::push_bounded(world.mp.url_requests, std::move(req),
                               game::kMaxUrlRequests);
        }
        return NativeDispatch::handled;
    }

    // -----------------------------------------------------------------
    // spawn effectors and network moderation (6 opcodes)
    // -----------------------------------------------------------------
    case 426: // set_spawn_effector_scene_prop_kind (0x1401c879a)
        if (world.is_valid_team(narrow[0]) &&
            static_cast<std::size_t>(narrow[0]) <
                world.fx.spawn_effector_scene_prop_kinds.size() &&
            (narrow[1] == -1 || world.is_valid_scene_prop(narrow[1]))) {
            world.fx.spawn_effector_scene_prop_kinds[static_cast<std::size_t>(
                narrow[0])] = narrow[1];
        }
        return NativeDispatch::handled;
    case 427: // set_spawn_effector_scene_prop_id (0x1401c87de)
        if (narrow[0] >= 0 && narrow[0] < game::kMaxSpawnEffectorSlots &&
            (narrow[1] == -1 || world.is_valid_mission_object(narrow[1]))) {
            world.fx.spawn_effector_mission_object_nos[static_cast<std::size_t>(
                narrow[0])] = narrow[1];
        }
        return NativeDispatch::handled;
    case 465: // kick_player (0x1401c8f59)
        if (world.is_valid_player(narrow[0]) && world.is_server &&
            narrow[0] != 0) {
            auto& pl = world.players[static_cast<std::size_t>(narrow[0])];
            if (pl.privilege < 3) {
                (void)world.disconnect_player(narrow[0]);
            }
        }
        return NativeDispatch::handled;
    case 466: // ban_player (0x1401c8fb7)
        if (world.is_valid_player(narrow[0]) && narrow[0] != 0 &&
            (narrow[1] > 0 || world.is_valid_player(narrow[2])) &&
            world.is_server) {
            auto& pl = world.players[static_cast<std::size_t>(narrow[0])];
            if (pl.privilege < 3) {
                native_world_push_ban_record(world, narrow[0], pl.unique_id,
                                             pl.name, narrow[1] > 0, narrow[2]);
                (void)world.disconnect_player(narrow[0]);
            }
        }
        return NativeDispatch::handled;
    case 467: // save_ban_info_of_player (0x1401c9082)
        if (world.is_valid_player(narrow[0]) && narrow[0] != 0 &&
            world.is_server) {
            const auto& pl =
                world.players[static_cast<std::size_t>(narrow[0])];
            if (pl.privilege < 3) {
                world.mp.saved_ban = game::WseBanInfo{
                    true, narrow[0], pl.unique_id, pl.name, true};
            }
        }
        return NativeDispatch::handled;
    case 468: // ban_player_using_saved_ban_info (0x1401c9119)
        if (world.mp.saved_ban.valid && world.is_server) {
            world.mp.saved_ban.valid = false;
            native_world_push_ban_record(world, world.mp.saved_ban.player_no,
                                         world.mp.saved_ban.unique_id,
                                         world.mp.saved_ban.name, true, -1);
            const std::int32_t bp = world.mp.saved_ban.player_no;
            if (bp >= 0 && bp < world.max_players &&
                world.is_valid_player(bp)) {
                (void)world.disconnect_player(bp);
            }
        }
        return NativeDispatch::handled;

    // -----------------------------------------------------------------
    // multiplayer launch, audio, positions, UI, and difficulty (13 opcodes)
    // -----------------------------------------------------------------
    case 470: // start_multiplayer_mission (0x1401c91c2)
        if (world.is_valid_site(narrow[1]) &&
            world.is_valid_mission_template(narrow[0])) {
            world.mp.started_manually = (narrow[2] != 0);
            world.mp.mission_starting = true;
            world.mp.next_site_no = narrow[1];
            world.mp.next_mission_template_no = narrow[0];
            if (world.is_multiplayer) {
                ctx.randSeed = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(ctx.randSeed) * 0x343fdU +
                    0x269ec3U);
                world.mp.mission_seed =
                    (static_cast<std::uint32_t>(ctx.randSeed) >> 16U) &
                    0x7fffU;
            }
        }
        return NativeDispatch::handled;
    case 616: // stop_sound_channel (0x1401caa78)
        if (narrow[0] >= 0) {
            if (world.audio_runtime != nullptr) {
                world.audio_runtime->stop_channel(narrow[0]);
            }
            if (narrow[0] < world.misc.num_sound_channels) {
                const auto ch = static_cast<std::size_t>(narrow[0]);
                if (world.music.stopped_channels.size() <= ch) {
                    world.music.stopped_channels.resize(ch + 1, false);
                }
                world.music.stopped_channels[ch] = true;
            }
        }
        return NativeDispatch::handled;
    case 702: // get_trigger_object_position (0x1401cac15)
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.fx.trigger_transform;
        }
        return NativeDispatch::handled;
    case 715: // get_sq_distance_between_position_heights (0x1401cb10e)
        if (wse_register_ok(narrow[1]) && wse_register_ok(narrow[2])) {
            const float dz =
                ctx.positions[static_cast<std::size_t>(narrow[1])].o.z -
                ctx.positions[static_cast<std::size_t>(narrow[2])].o.z;
            store_lvalue(dst, native_world_3_f2i(dz * dz * 100.0f), ctx, locals);
        } else {
            store_lvalue(dst, 0, ctx, locals);
        }
        return NativeDispatch::handled;
    case 960: // show_object_details_overlay (0x1401cd1ab)
        world.ui.show_object_details_overlay = (wide[0] != 0);
        return NativeDispatch::handled;
    case 970: // show_item_details (0x1401cd1ba)
        native_world_set_item_tooltip(world, ctx, narrow[0], 0, narrow[1],
                                      narrow[2]);
        return NativeDispatch::handled;
    case 971: // close_item_details (0x1401cd265)
        world.ui.item_tooltip.visible = false;
        return NativeDispatch::handled;
    case 972: // show_item_details_with_modifier (0x1401cd27b)
        if (narrow[1] >= 0) {
            native_world_set_item_tooltip(world, ctx, narrow[0], narrow[1],
                                          narrow[2], narrow[3]);
        }
        return NativeDispatch::handled;
    case 980: // context_menu_add_item (0x1401cd347)
        if (world.ui.context_menu_items.size() < game::kMaxContextMenuItems &&
            wse_string_register_ok(narrow[0], tag0, native_string_banks(ctx))) {
            game::WseContextMenuEntry item{};
            item.text =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx));
            item.value = narrow[1];
            game::push_bounded(world.ui.context_menu_items, std::move(item),
                               game::kMaxContextMenuItems);
        }
        return NativeDispatch::handled;
    case 985: // auto_save (0x1401cd3fc)
        ++world.misc.auto_save_count;
        return NativeDispatch::handled;
    case 988: // allow_ironman (0x1401cd456)
        world.misc.ironman_allowed = (narrow[0] != 0);
        return NativeDispatch::handled;
    case 990: { // get_average_game_difficulty (0x1401cd469)
        const std::int32_t cur = native_world_current_difficulty(ctx);
        const float cur_timer =
            static_cast<float>(static_cast<std::uint64_t>(ctx.timersTotal[0])) /
            100000.0f;
        const float prev_accum = world.misc.difficulty_time_accum;
        const float dt = cur_timer - world.misc.difficulty_last_timer;
        const float next_accum = prev_accum + dt;
        world.misc.difficulty_last_timer = cur_timer;
        world.misc.difficulty_time_accum = next_accum;
        std::int32_t avg = cur;
        if (next_accum > 0.0f && dt >= 0.0f) {
            const float weighted =
                (static_cast<float>(cur) * dt +
                 prev_accum * world.misc.average_game_difficulty) /
                next_accum;
            if (std::isfinite(weighted) && weighted > 0.0f) {
                avg = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    game::native_f2i_saturate(weighted), 0,
                    static_cast<std::int64_t>(
                        std::numeric_limits<std::int32_t>::max())));
            } else {
                avg = 0;
            }
        }
        world.misc.average_game_difficulty = static_cast<float>(avg);
        store_lvalue(dst, static_cast<std::int64_t>(avg), ctx, locals);
        return NativeDispatch::handled;
    }
    case 991: // get_level_boundary (0x1401cd4a6)
        if (narrow[1] >= 0 &&
            static_cast<std::size_t>(narrow[1]) <
                game::kCampaignLevelXpTable.size()) {
            const std::int32_t xp =
                game::kCampaignLevelXpTable[static_cast<std::size_t>(narrow[1])];
            store_lvalue(dst, native_world_3_round(static_cast<float>(xp)), ctx,
                         locals);
        }
        return NativeDispatch::handled;

    // -----------------------------------------------------------------
    // dialog, notes, quests, and encounters (19 opcodes)
    // -----------------------------------------------------------------
    case 1025: // start_map_conversation (0x1401cd61d)
        world.dialog.map_conversation_troop_no = narrow[0];
        world.dialog.map_conversation_troop_dna =
            (narrow[2] != 0) ? narrow[3] : 5;
        if (narrow[4] == 0) {
            world.dialog.map_conversation_active = true;
        }
        return NativeDispatch::handled;
    case 1090: // add_info_page_note_tableau_mesh (0x1401cd7d3)
        if (world.is_valid_info_page(narrow[0]) &&
            world.is_valid_tableau_material(narrow[1])) {
            world.info_pages[static_cast<std::size_t>(narrow[0])]
                .note_tableau_material_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1091: // add_info_page_note_from_dialog (0x1401cd813)
        if (ctx.currentCaller == 4 && world.is_valid_info_page(narrow[0])) {
            native_world_assign_dialog_note(
                world.info_pages[static_cast<std::size_t>(narrow[0])].notes,
                narrow[1], narrow[2] != 0, world);
        }
        return NativeDispatch::handled;
    case 1092: // add_info_page_note_from_sreg (0x1401cd907)
        if (world.is_valid_info_page(narrow[0])) {
            native_world_assign_sreg_note(
                world.info_pages[static_cast<std::size_t>(narrow[0])].notes,
                narrow[1], narrow[2], tag2, narrow[3] != 0, ctx);
        }
        return NativeDispatch::handled;
    case 1109: // add_faction_note_tableau_mesh (0x1401cddbe)
        if (world.is_valid_faction(narrow[0]) &&
            world.is_valid_tableau_material(narrow[1])) {
            world.factions[static_cast<std::size_t>(narrow[0])]
                .note_tableau_material_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1111: // add_quest_note_tableau_mesh (0x1401cde3e)
        if (world.is_valid_quest(narrow[0]) &&
            world.is_valid_tableau_material(narrow[1])) {
            world.quests[static_cast<std::size_t>(narrow[0])]
                .note_tableau_material_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1112: // add_quest_note_from_dialog (0x1401cde7e)
        if (ctx.currentCaller == 4 && world.is_valid_quest(narrow[0])) {
            native_world_assign_dialog_note(
                world.quests[static_cast<std::size_t>(narrow[0])].notes,
                narrow[1], narrow[2] != 0, world);
        }
        return NativeDispatch::handled;
    case 1115: // add_faction_note_from_dialog (0x1401ce126)
        if (ctx.currentCaller == 4 && world.is_valid_faction(narrow[0])) {
            native_world_assign_dialog_note(
                world.factions[static_cast<std::size_t>(narrow[0])].notes,
                narrow[1], narrow[2] != 0, world);
        }
        return NativeDispatch::handled;
    case 1118: // add_faction_note_from_sreg (0x1401ce3d4)
        if (world.is_valid_faction(narrow[0])) {
            native_world_assign_sreg_note(
                world.factions[static_cast<std::size_t>(narrow[0])].notes,
                narrow[1], narrow[2], tag2, narrow[3] != 0, ctx);
        }
        return NativeDispatch::handled;
    case 1130: // set_tooltip_text (0x1401ceaa9)
        if (wse_string_register_ok(narrow[0], tag0, native_string_banks(ctx))) {
            world.ui.tooltip_text = wse_utf16_to_utf8(
                native_resolve_and_parse(narrow[0], tag0, ctx));
        }
        return NativeDispatch::handled;
    case 1285: // set_quest_progression (0x1401cfee1)
        if (world.is_valid_quest(narrow[0])) {
            auto& q = world.quests[static_cast<std::size_t>(narrow[0])];
            q.progression_flags = (q.progression_flags & ~0xffU) |
                                  static_cast<std::uint32_t>(narrow[1] & 0xff);
        }
        return NativeDispatch::handled;
    case 1290: // setup_quest_text (0x1401cff37)
        if (world.is_valid_quest(narrow[0])) {
            auto& q = world.quests[static_cast<std::size_t>(narrow[0])];
            const std::string text_key =
                q.id.empty() ? std::string{} : q.id + "_text";
            const std::string parsed_text =
                native_world_parse_quest_text(world, text_key, q.text, ctx);
            q.text = parsed_text;
            world.misc.quest_strings[narrow[0]].text = parsed_text;
            q.title = native_world_parse_quest_text(world, q.id, q.title, ctx);
        }
        return NativeDispatch::handled;
    case 1291: // setup_quest_giver (0x1401cffde)
        if (world.is_valid_quest(narrow[0]) &&
            wse_string_register_ok(narrow[1], tag1, native_string_banks(ctx))) {
            world.misc.quest_strings[narrow[0]].giver_name = wse_utf16_to_utf8(
                native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    case 1300: { // start_encounter (0x1401d0069 -> 0x1400e0a30)
        if (world.is_valid_party(narrow[0])) {
            const std::int32_t p1 = (narrow[1] < 1)
                                        ? world.misc.camp_bandits_party_no
                                        : narrow[1];
            const bool p1_valid = world.is_valid_party(p1);
            if (!p1_valid && !(narrow[1] < 1 && p1 < 0)) {
                return NativeDispatch::handled;
            }
            const std::int32_t root0 = native_world_root_party_no(world, narrow[0]);
            if (p1_valid && p1 != world.misc.camp_bandits_party_no &&
                p1 == world.main_party_no) {
                if (narrow[0] == world.main_party_no &&
                    narrow[0] != world.misc.camp_bandits_party_no) {
                    return NativeDispatch::handled;
                }
                world.encountered_party_no = native_world_root_party_no(world, p1);
                world.encountered_party_2_no = -1;
                world.encountered_party_is_attacker = true;
                world.misc.repeat_object = 1;
            } else {
                world.encountered_party_no = root0;
                world.encountered_party_2_no = -1;
                world.encountered_party_is_attacker = false;
                world.misc.repeat_object = 0;
            }
        }
        return NativeDispatch::handled;
    }
    case 1301: // leave_encounter (0x1401d009d)
        // Own flag: save_mode_flags feeds native_world_current_difficulty.
        world.misc.encounter_left = true;
        return NativeDispatch::handled;
    case 1303: // select_enemy (0x1401d00b0)
        if (static_cast<std::uint32_t>(narrow[0]) < 2U) {
            world.main_party_side = 1 - narrow[0];
        }
        return NativeDispatch::handled;
    case 1304: // set_passage_menu (0x1401d00d6)
        if (world.is_valid_menu(narrow[0])) {
            world.misc.passage_menu_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 1305: // auto_set_meta_mission_at_end_commited (0x1401d00f5)
        world.misc.meta_mission_end_committed = false;
        return NativeDispatch::handled;
    case 1307: // end_current_battle (0x1401d0101)
        world.misc.meta_mission_event_ended = true;
        return NativeDispatch::handled;

    // -----------------------------------------------------------------
    // mission FX, missiles, tableau triggers, talk/menu UI (16 opcodes)
    // -----------------------------------------------------------------
    case 1805: { // ai_mesh_face_group_show_hide (0x1401d6a64)
        const bool visible = (wide[1] != 0);
        for (auto& face : world.fx.ai_mesh_faces) {
            if (face.group_id == narrow[0]) {
                face.visible = visible;
            }
        }
        return NativeDispatch::handled;
    }
    case 1829: // add_missile (0x1401d756b)
        if (world.is_valid_agent(narrow[0]) && wse_register_ok(narrow[1]) &&
            world.is_valid_item(narrow[3]) && world.is_valid_item(narrow[5])) {
            game::WseMissileSpawnEntry entry{};
            entry.shooter_agent_no = narrow[0];
            entry.launch_pos = ctx.positions[static_cast<std::size_t>(narrow[1])];
            entry.item_no = narrow[3];
            entry.item_modifier =
                (static_cast<std::uint32_t>(narrow[4]) <
                 static_cast<std::uint32_t>(kMaxNativeItemModifier))
                    ? narrow[4]
                    : 0;
            entry.missile_item_no = narrow[5];
            entry.missile_modifier =
                (static_cast<std::uint32_t>(narrow[6]) <
                 static_cast<std::uint32_t>(kMaxNativeItemModifier))
                    ? narrow[6]
                    : 0;
            entry.speed = static_cast<float>(wide[2]) / fp;
            game::push_bounded(world.fx.missiles, std::move(entry),
                               kMaxNativeMissileSpawns);
        }
        return NativeDispatch::handled;
    case 1891: // replace_scene_items_with_scene_props (0x1401d9188)
        if (world.is_valid_item(narrow[0]) &&
            world.is_valid_scene_prop(narrow[1]) &&
            static_cast<std::uint32_t>(ctx.currentCaller + 20) < 2U) {
            for (auto& obj : world.mission_objects) {
                if (obj.active && obj.is_item &&
                    obj.scene_prop_no == narrow[0]) {
                    obj.scene_prop_no = narrow[1];
                    obj.is_item = false;
                    obj.m5.is_spawned_item = false;
                }
            }
        }
        return NativeDispatch::handled;
    case 1920: { // start_mission_conversation (0x1401d95fb)
        std::int32_t found = -1;
        for (std::size_t i = 0; i < world.agents.size(); ++i) {
            if (world.agents[i].active &&
                world.agents[i].troop_no == narrow[0]) {
                found = static_cast<std::int32_t>(i);
                break;
            }
        }
        world.dialog.pending_mission_agent_no = found;
        return NativeDispatch::handled;
    }
    case 1950: // set_current_color (0x1401d9865)
        world.fx.trigger_current_color = wse::core::WseVec4{
            static_cast<float>(wide[0]) / 100.0f,
            static_cast<float>(wide[1]) / 100.0f,
            static_cast<float>(wide[2]) / 100.0f, 1.0f};
        return NativeDispatch::handled;
    case 1955: // set_position_delta (0x1401d98db)
        world.fx.trigger_position_delta = wse::core::WseVec4{
            static_cast<float>(wide[0]) / 100.0f,
            static_cast<float>(wide[1]) / 100.0f,
            static_cast<float>(wide[2]) / 100.0f, 0.0f};
        return NativeDispatch::handled;
    case 1982: // cur_scene_prop_set_tableau_material (0x1401da591)
        if (world.is_valid_tableau_material(narrow[0]) &&
            ctx.currentCaller == -40) {
            world.fx.cur_scene_prop_tableau_material_no = narrow[0];
            world.fx.cur_scene_prop_tableau_param = narrow[1];
        }
        return NativeDispatch::handled;
    case 1983: // cur_map_icon_set_tableau_material (0x1401da619)
        if (world.is_valid_tableau_material(narrow[0]) &&
            ctx.currentCaller == -70) {
            world.fx.cur_map_icon_tableau_material_no = narrow[0];
            world.fx.cur_map_icon_tableau_param = narrow[1];
        }
        return NativeDispatch::handled;
    case 1986: // cur_agent_set_banner_tableau_material (0x1401da687)
        if (world.is_valid_tableau_material(narrow[0]) &&
            world.is_valid_agent(world.fx.trigger_agent_no)) {
            auto& ag =
                world.agents[static_cast<std::size_t>(world.fx.trigger_agent_no)];
            ag.banner_tableau_material_no = narrow[0];
            ag.banner_tableau_param = narrow[1];
            ag.show_banner = true;
        }
        return NativeDispatch::handled;
    case 2020: // talk_info_show (0x1401dbeca)
        world.dialog.show_talk_info = (narrow[0] != 0);
        return NativeDispatch::handled;
    case 2021: // talk_info_set_relation_bar (0x1401dbee2)
        world.dialog.talk_info_relation_value = narrow[0];
        return NativeDispatch::handled;
    case 2022: // talk_info_set_line (0x1401dbef5)
        if (narrow[0] >= 0 && narrow[0] < game::kMaxTalkInfoLines &&
            wse_string_register_ok(narrow[1], tag1, native_string_banks(ctx))) {
            world.dialog.talk_info_lines[static_cast<std::size_t>(narrow[0])] =
                wse_utf16_to_utf8(
                    native_resolve_and_parse(narrow[1], tag1, ctx));
            world.dialog.show_talk_info = true;
        }
        return NativeDispatch::handled;
    case 2031: // set_background_mesh (0x1401dbf8a)
        if (world.is_valid_mesh(narrow[0]) &&
            world.is_valid_menu(world.ui.current_menu_no)) {
            world.ui.background_mesh_name =
                world.mesh_names[static_cast<std::size_t>(narrow[0])];
        }
        return NativeDispatch::handled;
    case 2032: // set_game_menu_tableau_mesh (0x1401dbfdf)
        if (world.is_valid_tableau_material(narrow[0]) &&
            world.is_valid_menu(world.ui.current_menu_no) &&
            wse_register_ok(narrow[2])) {
            world.ui.menu_tableau_material_no = narrow[0];
            world.ui.menu_tableau_param = static_cast<float>(narrow[1]);
            world.ui.menu_tableau_bounds =
                ctx.positions[static_cast<std::size_t>(narrow[2])].o;
        }
        return NativeDispatch::handled;
    case 2061: // disable_menu_option (0x1401dc474)
        world.ui.current_menu_option_disabled = true;
        return NativeDispatch::handled;
    case 2198: // set_conversation_speaker_agent (0x1401dddf5)
        if (world.is_valid_agent(narrow[0])) {
            world.dialog.speaker_agent_no = narrow[0];
        }
        return NativeDispatch::handled;

    // -----------------------------------------------------------------
    // lighting, skybox, and postfx (10 opcodes)
    // -----------------------------------------------------------------
    case 2386: // set_postfx (0x1401df83c)
        if (world.is_valid_post_effect(narrow[0])) {
            world.lighting.active_postfx_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 2387: // set_river_shader_to_mud (0x1401df866)
        if (world.lighting.has_scene) {
            world.lighting.river_shader_mud = true;
        }
        return NativeDispatch::handled;
    case 2389: // set_skybox (0x1401df980)
        world.lighting.hdr_skybox_no = narrow[1];
        world.lighting.skybox_no = narrow[0];
        return NativeDispatch::handled;
    case 2390: // set_startup_sun_light (0x1401df99d)
        native_world_set_fp_rgb(wide, fp, world.lighting.sun_color);
        return NativeDispatch::handled;
    case 2391: // set_startup_ambient_light (0x1401dfa6f)
        native_world_set_fp_rgb(wide, fp, world.lighting.ambient_color);
        if (world.lighting.has_scene) {
            world.lighting.ambient_color.w = 1.0f;
            world.lighting.ground_ambient_color.w = 1.0f;
        }
        return NativeDispatch::handled;
    case 2392: // set_startup_ground_ambient_light (0x1401dfb40)
        native_world_set_fp_rgb(wide, fp, world.lighting.ground_ambient_color);
        if (world.lighting.has_scene) {
            world.lighting.ambient_color.w = 1.0f;
            world.lighting.ground_ambient_color.w = 1.0f;
        }
        return NativeDispatch::handled;
    case 2393: // rebuild_shadow_map (0x1401dfbf7)
        world.lighting.shadow_map_needs_restore = true;
        return NativeDispatch::handled;
    case 2394: // get_startup_sun_light (0x1401dfc03)
        native_world_store_light_rgb(narrow[0], world.lighting.has_ambient_light,
                                     world.lighting.sun_color, true, ctx);
        return NativeDispatch::handled;
    case 2395: // get_startup_ambient_light (0x1401dfc70)
        native_world_store_light_rgb(narrow[0], world.lighting.has_scene,
                                     world.lighting.ambient_color, false, ctx);
        return NativeDispatch::handled;
    case 2396: // get_startup_ground_ambient_light (0x1401dfcd6)
        native_world_store_light_rgb(narrow[0], world.lighting.has_scene,
                                     world.lighting.ground_ambient_color, false,
                                     ctx);
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_agent_statement(opcode, wide, narrow, tags, statement, ctx,
                                           locals);
}

}  // namespace wse::vm

#include "wse/vm/native_agent_ops.h"
