// Native entity, campaign, presentation, site, quest, troop, and party opcode handlers
// (v1.1.5.5 mbStatement::execute).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_mission_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

[[nodiscard]] inline std::u16string native_resolve_and_parse(std::int32_t str_idx,
                                                             std::uint8_t tag,
                                                             VmContext& ctx) {
    const auto banks = native_string_banks(ctx);
    const std::u16string_view raw = wse_resolve_string(str_idx, tag, banks);
    return wse_parse_string(raw, ctx.registers, banks);
}

[[nodiscard]] inline float native_entity_randf(VmContext& ctx) noexcept {
    const auto seed = static_cast<std::uint32_t>(ctx.randSeed) * 0x343fdU + 0x269ec3U;
    ctx.randSeed = static_cast<std::int32_t>(seed);
    return static_cast<float>(((seed >> 16U) & 0x7fffU) % 0x3dc9U) / 15817.0f;
}

inline void native_record_mp_message(std::int32_t player_no,
                                     std::int32_t msg_type,
                                     std::span<const std::int64_t> ints,
                                     const std::u16string& text,
                                     VmContext& ctx) {
    if (!ctx.world.is_valid_player(player_no)) {
        return;
    }
    game::WseMultiplayerMessage msg{};
    msg.player_no = player_no;
    msg.event_type = msg_type;
    msg.num_ints = static_cast<std::int32_t>(std::min<std::size_t>(ints.size(), 4U));
    for (std::int32_t i = 0; i < msg.num_ints; ++i) {
        msg.ints[static_cast<std::size_t>(i)] = ints[static_cast<std::size_t>(i)];
    }
    msg.text = text;
    ctx.world.multiplayer_messages.push_back(std::move(msg));
}

[[nodiscard]] inline game::WseInventorySlot* native_troop_slot_ptr(game::WseTroop& troop,
                                                                   std::int32_t slot_no,
                                                                   const game::WseWorldState& world) noexcept {
    const std::int32_t inv_slots = troop.get_num_inventory_slots(
        world.skills[static_cast<std::size_t>(game::kSkillInventoryManagement)].max_level);
    if (slot_no < 0 || slot_no >= inv_slots + game::kNumTroopEquipmentSlots ||
        static_cast<std::size_t>(slot_no) >= troop.inventory.size()) {
        return nullptr;
    }
    return &troop.inventory[static_cast<std::size_t>(slot_no)];
}

inline void native_loot_copy_slot(game::WseTroop& dst_troop,
                                  std::int32_t dst_slot_no,
                                  const game::WseInventorySlot& item) {
    if (dst_slot_no >= game::kNumTroopEquipmentSlots &&
        static_cast<std::size_t>(dst_slot_no) < dst_troop.inventory.size()) {
        dst_troop.inventory[static_cast<std::size_t>(dst_slot_no)] = item;
    }
}

[[nodiscard]] inline NativeDispatch dispatch_native_entity_statement(
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
    // Player & Multiplayer
    case 47: // set_player_troop (0x1401c6548)
        if (world.is_valid_troop(narrow[0])) {
            world.player_troop_no = narrow[0];
        }
        return NativeDispatch::handled;

    case 394: // multiplayer_send_message_to_player (0x1401c7958)
        native_record_mp_message(narrow[0], narrow[1], {}, {}, ctx);
        return NativeDispatch::handled;
    case 395: // multiplayer_send_int_to_player (0x1401c7a0d)
        native_record_mp_message(narrow[0], narrow[1], wide.subspan(2, 1), {}, ctx);
        return NativeDispatch::handled;
    case 396: // multiplayer_send_2_int_to_player (0x1401c7aca)
        native_record_mp_message(narrow[0], narrow[1], wide.subspan(2, 2), {}, ctx);
        return NativeDispatch::handled;
    case 397: // multiplayer_send_3_int_to_player (0x1401c7b92)
        native_record_mp_message(narrow[0], narrow[1], wide.subspan(2, 3), {}, ctx);
        return NativeDispatch::handled;
    case 398: // multiplayer_send_4_int_to_player (0x1401c7c50)
        native_record_mp_message(narrow[0], narrow[1], wide.subspan(2, 4), {}, ctx);
        return NativeDispatch::handled;
    case 399: { // multiplayer_send_string_to_player (0x1401c7d19)
        const std::uint8_t tag2 = wse_tag_at(tags, 2);
        if (world.is_valid_player(narrow[0]) && wse_string_register_ok(narrow[2], tag2, banks)) {
            const std::u16string parsed = native_resolve_and_parse(narrow[2], tag2, ctx);
            if (narrow[0] == 0) {
                ctx.stringRegisters[0] = parsed;
            }
            native_record_mp_message(narrow[0], narrow[1], {}, parsed, ctx);
        }
        return NativeDispatch::handled;
    }

    case 400: // get_max_players (0x1401c7e4c)
        store_lvalue(dst, static_cast<std::int64_t>(world.max_players), ctx, locals);
        return NativeDispatch::handled;
    case 401: // player_is_active (0x1401c7e73)
        return (narrow[0] >= 0 && narrow[0] < world.max_players &&
                world.is_valid_player(narrow[0]))
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 402: // player_get_team_no (0x1401c7ec6)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].team_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 403: // player_set_team_no (0x1401c7f0d)
        if (world.is_valid_player(narrow[0]) && narrow[1] >= 0 && narrow[1] < 4) {
            world.players[static_cast<std::size_t>(narrow[0])].team_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 404: // player_get_troop_id (0x1401c7f6a)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].troop_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 405: // player_set_troop_id (0x1401c7fb1)
        if (world.is_valid_player(narrow[0]) &&
            (narrow[1] == -1 || world.is_valid_troop(narrow[1]))) {
            world.players[static_cast<std::size_t>(narrow[0])].troop_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 406: // player_get_agent_id (0x1401c8013)
        if (world.is_valid_player(narrow[1])) {
            auto& player = world.players[static_cast<std::size_t>(narrow[1])];
            if (!world.is_valid_agent(player.agent_no)) {
                player.agent_no = -1;
            }
            store_lvalue(dst, static_cast<std::int64_t>(player.agent_no), ctx, locals);
        }
        return NativeDispatch::handled;
    case 407: // player_get_gold (0x1401c809e)
        if (world.is_valid_player(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.players[static_cast<std::size_t>(narrow[1])].gold),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 408: // player_set_gold (0x1401c80e5)
        if (world.is_valid_player(narrow[0]) && narrow[1] >= 0) {
            const std::int32_t cap = narrow[2] > 0 ? narrow[2] : 1000000000;
            world.players[static_cast<std::size_t>(narrow[0])].gold =
                std::min(narrow[1], cap);
        }
        return NativeDispatch::handled;
    case 430: // player_is_admin (0x1401c86ed)
        return (world.is_valid_player(narrow[0]) &&
                world.players[static_cast<std::size_t>(narrow[0])].is_admin)
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;

    // Quests
    case 200: // check_quest_active (0x1401c6f8c)
        return (world.is_valid_quest(narrow[0]) &&
                world.quests[static_cast<std::size_t>(narrow[0])].is_active())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 201: // check_quest_finished (0x1401c6fb9)
        return (world.is_valid_quest(narrow[0]) &&
                world.quests[static_cast<std::size_t>(narrow[0])].is_completed())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 202: // check_quest_succeeded (0x1401c6fe6)
        return (world.is_valid_quest(narrow[0]) &&
                world.quests[static_cast<std::size_t>(narrow[0])].is_succeeded())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 203: // check_quest_failed (0x1401c7013)
        return (world.is_valid_quest(narrow[0]) &&
                world.quests[static_cast<std::size_t>(narrow[0])].is_failed())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 204: // check_quest_concluded (0x1401c7040)
        return (world.is_valid_quest(narrow[0]) &&
                world.quests[static_cast<std::size_t>(narrow[0])].is_concluded())
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1113: { // add_quest_note_from_sreg (0x1401cdde9)
        const std::uint8_t tag2 = wse_tag_at(tags, 2);
        if (wse_string_register_ok(narrow[2], tag2, banks) &&
            world.is_valid_quest(narrow[0]) &&
            narrow[1] >= 0 && narrow[1] < game::kNumQuestNotes) {
            auto& note =
                world.quests[static_cast<std::size_t>(narrow[0])]
                    .notes[static_cast<std::size_t>(narrow[1])];
            note.text = native_resolve_and_parse(narrow[2], tag2, ctx);
            note.date = (narrow[3] == 0) ? -1 : world.current_day;
        }
        return NativeDispatch::handled;
    }
    case 1280: // start_quest (0x1401cfc8b)
        if (world.is_valid_quest(narrow[0])) {
            auto& q = world.quests[static_cast<std::size_t>(narrow[0])];
            q.giver_troop_no =
                (narrow[1] >= 1 && world.is_valid_troop(narrow[1]))
                    ? narrow[1]
                    : world.conversation_partner_troop_no;
            if ((q.template_flags & 2U) != 0U || !q.is_active()) {
                q.progression_flags = 0x100U;
            }
        }
        return NativeDispatch::handled;
    case 1281: // complete_quest (0x1401cfccd)
        if (world.is_valid_quest(narrow[0])) {
            auto& q = world.quests[static_cast<std::size_t>(narrow[0])];
            if ((q.template_flags & 2U) != 0U) {
                q.progression_flags = 0U;
            } else if (q.is_active() && !q.is_completed()) {
                q.progression_flags = (q.progression_flags & ~0x100U) | 0x200U;
            }
        }
        return NativeDispatch::handled;
    case 1282: // succeed_quest (0x1401cfcf2)
        if (world.is_valid_quest(narrow[0])) {
            world.quests[static_cast<std::size_t>(narrow[0])].progression_flags |= 0x1400U;
        }
        return NativeDispatch::handled;
    case 1283: // fail_quest (0x1401cfd17)
        if (world.is_valid_quest(narrow[0])) {
            world.quests[static_cast<std::size_t>(narrow[0])].progression_flags |= 0x1800U;
        }
        return NativeDispatch::handled;
    case 1284: // cancel_quest (0x1401cfd3c)
        if (world.is_valid_quest(narrow[0])) {
            world.quests[static_cast<std::size_t>(narrow[0])].progression_flags = 0U;
        }
        return NativeDispatch::handled;
    case 1286: // conclude_quest (0x1401cfd92)
        if (world.is_valid_quest(narrow[0])) {
            world.quests[static_cast<std::size_t>(narrow[0])].progression_flags |= 0x1000U;
        }
        return NativeDispatch::handled;

    // Audio, Terrain, Presentation, UI
    case 599: // play_sound_at_position (0x1401ca66e)
        if (narrow[0] >= 0 && wse_register_ok(narrow[1])) {
            const wse::core::WseVec4 pos =
                ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            world.played_sounds.push_back(
                game::WseBattleEvent{0, narrow[0], -1, narrow[2], pos});
            if (world.audio_runtime != nullptr) {
                world.music.last_sound_channel = world.audio_runtime->play_sound(
                    narrow[0], pos, static_cast<std::uint32_t>(narrow[2]), 1.0f, 0);
            }
        }
        return NativeDispatch::handled;
    case 600: // play_sound (0x1401ca75d)
        if (narrow[0] >= 0) {
            world.played_sounds.push_back(
                game::WseBattleEvent{0, narrow[0], -1, narrow[1], {}});
            if (world.audio_runtime != nullptr) {
                world.music.last_sound_channel = world.audio_runtime->play_sound_2d(
                    narrow[0], static_cast<std::uint32_t>(narrow[1]));
            }
        }
        return NativeDispatch::handled;
    case 791: // position_set_z_to_ground_level (0x1401cbd9b)
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])].o.z = world.ground_level_z;
        }
        return NativeDispatch::handled;
    case 900: // start_presentation (0x1401cbf02)
        // The queued 0x0f entry is consumed only by `mbMapWindow::frameMove` (0x140117f90), which
        // `step_world_frame` runs on campaign frames alone. During a mission (safety deviation:
        // retail's mission-side path is not reconstructed) only `active_presentation_no` is set,
        // since a queued entry would never drain there and would reopen a stale presentation
        // once the player is back on the map.
        if (narrow[0] >= 0) {
            world.active_presentation_no = narrow[0];
            world.presentation_background = false;
            ++world.m6.ui.presentation_start_serial;
            if (!world.lighting.has_scene) {
                world.enqueue_window(game::kQueuedWindowPresentationType, narrow[0]);
            }
        }
        return NativeDispatch::handled;
    case 901: // start_background_presentation (0x1401cbf2d)
        if (ctx.currentCaller == 8 && narrow[0] >= 0) {
            world.active_presentation_no = narrow[0];
            world.presentation_background = true;
        }
        return NativeDispatch::handled;
    case 902: // presentation_set_duration (0x1401cbf60)
        if (statement.numOperands < 2 ||
            (narrow[1] >= 0 && narrow[1] == world.active_presentation_no)) {
            world.presentation_duration = static_cast<std::int32_t>(wide[0]);
        } else if (world.lighting.has_scene && narrow[1] >= 0) {
            // Mission branch (`0x1401cc016`): the conversation window's container for that
            // presentation (`getPresentationContainer` `0x140088f00`) gets the duration.
            world.m6.ui.mission_duration_requests.emplace_back(
                narrow[1], static_cast<std::int32_t>(std::clamp<std::int64_t>(
                               wide[0], 0, std::numeric_limits<std::int32_t>::max())));
        }
        return NativeDispatch::handled;
    case 903: { // is_presentation_active (0x1401cc09d)
        if (narrow[0] >= 0 && world.active_presentation_no == narrow[0]) {
            return NativeDispatch::handled;
        }
        // Mission branch (`0x1401cc135`): any open conversation-window container for it.
        const auto& open = world.m6.ui.active_presentations;
        const bool open_in_mission =
            world.lighting.has_scene && narrow[0] >= 0 &&
            std::find(open.begin(), open.end(), narrow[0]) != open.end();
        return open_in_mission ? NativeDispatch::handled : NativeDispatch::condition_false;
    }
    case 910: { // create_text_overlay (0x1401cc194)
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (world.active_presentation_no >= 0 && wse_string_register_ok(narrow[1], tag1, banks)) {
            game::WsePresentationOverlay ov{};
            ov.type = 1;
            ov.text = native_resolve_and_parse(narrow[1], tag1, ctx);
            ov.val = narrow[2];
            // Like every create opcode, the overlay joins the `set_container_overlay` (945)
            // container (`createTextOverlay` `0x140209a90` adds it to that overlay's widget).
            ov.m5.container_overlay_no = world.m5.current_container_overlay_no;
            ov.m5.create_opcode = 910;
            const auto id = static_cast<std::int64_t>(world.overlays.size());
            world.overlays.push_back(std::move(ov));
            store_lvalue(dst, id, ctx, locals);
        }
        return NativeDispatch::handled;
    }
    case 920: { // overlay_set_text (0x1401cc511)
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (world.is_valid_overlay(narrow[0]) && wse_string_register_ok(narrow[1], tag1, banks)) {
            auto& ov = world.overlays[static_cast<std::size_t>(narrow[0])];
            ov.text = native_resolve_and_parse(narrow[1], tag1, ctx);
            // `setOverlayText` (`0x14020df90`) -> `rglWidget::setText` (`0x14031e330`), which
            // re-measures a 912 button box at the current text size.
            ov.m5.text_measure_scale = ov.size;
        }
        return NativeDispatch::handled;
    }
    case 921: // overlay_set_color (0x1401cc592)
        if (world.is_valid_overlay(narrow[0])) {
            world.overlays[static_cast<std::size_t>(narrow[0])].color =
                static_cast<std::uint32_t>(narrow[1]);
        }
        return NativeDispatch::handled;
    case 922: // overlay_set_alpha (0x1401cc5bc)
        if (world.is_valid_overlay(narrow[0])) {
            world.overlays[static_cast<std::size_t>(narrow[0])].alpha =
                static_cast<float>(narrow[1]) / 255.0f;
        }
        return NativeDispatch::handled;
    case 925: // overlay_set_size (0x1401cc63a)
        if (world.is_valid_overlay(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& o = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            world.overlays[static_cast<std::size_t>(narrow[0])].size = {o.x, o.y};
            world.overlays[static_cast<std::size_t>(narrow[0])].m5.size_set = true;
        }
        return NativeDispatch::handled;
    case 926: // overlay_set_position (0x1401cc698)
        if (world.is_valid_overlay(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& o = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            world.overlays[static_cast<std::size_t>(narrow[0])].position = {o.x, o.y};
        }
        return NativeDispatch::handled;
    case 927: // overlay_set_val (0x1401cc6f6)
        if (world.is_valid_overlay(narrow[0])) {
            world.overlays[static_cast<std::size_t>(narrow[0])].val = narrow[1];
        }
        return NativeDispatch::handled;
    case 947: // overlay_set_display (0x1401ccdb6)
        if (world.is_valid_overlay(narrow[0])) {
            world.overlays[static_cast<std::size_t>(narrow[0])].visible = (narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 1107: // set_show_messages (0x1401cdbef)
        world.show_messages = (wide[0] != 0);
        return NativeDispatch::handled;

    // Sites, Visitors, Factions
    case 1232: // remove_party (0x1401cf49f)
        (void)world.remove_party(narrow[0]);
        return NativeDispatch::handled;
    case 1261: // modify_visitors_at_site (0x1401cf657)
        if (world.is_valid_site(narrow[0])) {
            world.site_no_being_modified = narrow[0];
        }
        return NativeDispatch::handled;
    case 1262: // reset_visitors (0x1401cf676)
        if (world.is_valid_site(world.site_no_being_modified)) {
            world.sites[static_cast<std::size_t>(world.site_no_being_modified)]
                .visitors.clear();
        }
        return NativeDispatch::handled;
    case 1263: // set_visitor (0x1401cf6a8)
        if (world.is_valid_site(world.site_no_being_modified) &&
            narrow[0] >= 0 && narrow[0] < game::kMaxEntryPoints) {
            auto& visitors =
                world.sites[static_cast<std::size_t>(world.site_no_being_modified)].visitors;
            if (wide[1] == -1) {
                std::erase_if(visitors, [entry = narrow[0]](const game::WseSiteVisitor& v) {
                    return v.entry_no == entry;
                });
            } else if (world.is_valid_troop(narrow[1])) {
                visitors.push_back(game::WseSiteVisitor{
                    narrow[0], narrow[1], (wide[2] != 0) ? narrow[2] : -0xd0cd});
            }
        }
        return NativeDispatch::handled;
    case 1264: // set_visitors (0x1401cf746)
        if (world.is_valid_site(world.site_no_being_modified) &&
            narrow[0] >= 0 && narrow[0] < game::kMaxEntryPoints &&
            world.is_valid_troop(narrow[1]) && narrow[2] > 0) {
            auto& visitors =
                world.sites[static_cast<std::size_t>(world.site_no_being_modified)].visitors;
            for (std::int32_t i = 0; i < narrow[2]; ++i) {
                visitors.push_back(game::WseSiteVisitor{narrow[0], narrow[1], -0xd0cd});
            }
        }
        return NativeDispatch::handled;
    case 1270: // set_relation (0x1401cf910)
        if (world.is_valid_faction(narrow[0]) && world.is_valid_faction(narrow[1])) {
            const float rel = std::clamp(static_cast<float>(narrow[2]) / 100.0f, -1.0f, 1.0f);
            world.factions[static_cast<std::size_t>(narrow[0])].set_relation(
                static_cast<std::size_t>(narrow[1]), rel);
            world.factions[static_cast<std::size_t>(narrow[1])].set_relation(
                static_cast<std::size_t>(narrow[0]), rel);
        }
        return NativeDispatch::handled;

    // Troops
    case 1505: // troop_set_type (0x1401d01f0)
        if (world.is_valid_troop(narrow[0])) {
            const std::int32_t hi = std::max<std::int32_t>(0, world.num_skins - 1);
            world.troops[static_cast<std::size_t>(narrow[0])].set_skin_no(
                std::clamp(narrow[1], 0, hi));
        }
        return NativeDispatch::handled;
    case 1506: // troop_get_type (0x1401d0233)
        if (world.is_valid_troop(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.troops[static_cast<std::size_t>(narrow[1])].get_skin_no()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1509: // troop_set_auto_equip (0x1401d02d0)
        if (world.is_valid_troop(narrow[0])) {
            auto& tr = world.troops[static_cast<std::size_t>(narrow[0])];
            if (wide[1] == 0) {
                tr.flags |= 0x1000ULL;
                tr.auto_equip = false;
            } else {
                tr.flags &= ~0x1000ULL;
                tr.auto_equip = true;
            }
        }
        return NativeDispatch::handled;
    case 1516: // troop_get_class (0x1401d043d)
        if (world.is_valid_troop(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.troops[static_cast<std::size_t>(narrow[1])].class_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1517: // troop_set_class (0x1401d0484)
        if (world.is_valid_troop(narrow[0])) {
            world.troops[static_cast<std::size_t>(narrow[0])].class_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1520: // troop_raise_attribute (0x1401d04b3)
        if (world.is_valid_troop(narrow[0]) && narrow[2] != 0 &&
            narrow[1] >= 0 && narrow[1] < game::kMaxAttributes) {
            world.troops[static_cast<std::size_t>(narrow[0])].raise_attribute(
                narrow[1], narrow[2],
                world.skills[static_cast<std::size_t>(game::kSkillIronflesh)].max_level,
                world.attribute_limit);
        }
        return NativeDispatch::handled;
    case 1521: // troop_raise_skill (0x1401d050b)
        if (world.is_valid_troop(narrow[0]) && narrow[2] != 0 &&
            narrow[1] >= 0 && narrow[1] < game::kMaxSkills) {
            world.troops[static_cast<std::size_t>(narrow[0])].raise_skill(
                narrow[1], narrow[2],
                world.skills[static_cast<std::size_t>(narrow[1])].max_level,
                world.skills[static_cast<std::size_t>(game::kSkillIronflesh)].max_level);
        }
        return NativeDispatch::handled;
    case 1522: // troop_raise_proficiency (0x1401d0563)
        if (world.is_valid_troop(narrow[0]) && wide[2] >= 0 &&
            narrow[1] >= 0 && narrow[1] < game::kMaxProficiencies) {
            auto& prof = world.troops[static_cast<std::size_t>(narrow[0])]
                             .proficiencies[static_cast<std::size_t>(narrow[1])];
            const float gain = static_cast<float>(wide[2]) * (100.0f / (prof + 100.0f));
            prof = std::clamp(prof + gain, 0.0f, 1000.0f);
        }
        return NativeDispatch::handled;
    case 1523: // troop_raise_proficiency_linear (0x1401d05ca)
        if (world.is_valid_troop(narrow[0]) && narrow[2] != 0 &&
            narrow[1] >= 0 && narrow[1] < game::kMaxProficiencies) {
            world.troops[static_cast<std::size_t>(narrow[0])]
                .raise_proficiency_linear(narrow[1], narrow[2]);
        }
        return NativeDispatch::handled;
    case 1528: // troop_add_gold (0x1401d066a)
        if (world.is_valid_troop(narrow[0]) && narrow[1] != 0) {
            auto& g = world.troops[static_cast<std::size_t>(narrow[0])].gold;
            g = std::max<std::int32_t>(0, g + narrow[1]);
        }
        return NativeDispatch::handled;
    case 1529: // troop_remove_gold (0x1401d069c)
        if (world.is_valid_troop(narrow[0]) && narrow[1] > 0) {
            auto& g = world.troops[static_cast<std::size_t>(narrow[0])].gold;
            g = std::max<std::int32_t>(0, g - narrow[1]);
        }
        return NativeDispatch::handled;
    case 1530: // troop_add_item (0x1401d06ce)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_item(narrow[1])) {
            auto& tr = world.troops[static_cast<std::size_t>(narrow[0])];
            const std::int32_t free_slot = tr.get_first_free_inventory_slot(
                world.skills[static_cast<std::size_t>(game::kSkillInventoryManagement)]
                    .max_level);
            if (free_slot >= 0) {
                tr.set_item(free_slot, narrow[1], narrow[2],
                            world.items[static_cast<std::size_t>(narrow[1])].max_ammo);
            }
        }
        return NativeDispatch::handled;
    case 1532: // troop_clear_inventory (0x1401d0859)
        if (world.is_valid_troop(narrow[0])) {
            world.troops[static_cast<std::size_t>(narrow[0])].clear_inventory();
        }
        return NativeDispatch::handled;
    case 1535: // troop_add_items (0x1401d094b)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_item(narrow[1]) &&
            narrow[2] > 0) {
            auto& tr = world.troops[static_cast<std::size_t>(narrow[0])];
            const std::int32_t inv_max =
                world.skills[static_cast<std::size_t>(game::kSkillInventoryManagement)]
                    .max_level;
            const std::int32_t ammo =
                world.items[static_cast<std::size_t>(narrow[1])].max_ammo;
            for (std::int32_t i = 0; i < narrow[2]; ++i) {
                const std::int32_t free_slot = tr.get_first_free_inventory_slot(inv_max);
                if (free_slot < 0) {
                    break;
                }
                tr.set_item(free_slot, narrow[1], 0, ammo);
            }
        }
        return NativeDispatch::handled;
    case 1536: // troop_remove_items (0x1401d0a47)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_item(narrow[1]) &&
            narrow[2] > 0) {
            auto& tr = world.troops[static_cast<std::size_t>(narrow[0])];
            const std::int32_t bag_cap = tr.get_num_inventory_slots(
                world.skills[static_cast<std::size_t>(game::kSkillInventoryManagement)]
                    .max_level);
            std::int32_t removed = 0;
            std::int64_t value_sum = 0;
            const auto item_val = static_cast<std::int64_t>(
                world.items[static_cast<std::size_t>(narrow[1])].value);
            auto try_remove_range = [&](std::int32_t start, std::int32_t end) {
                for (std::int32_t s = start; s < end && removed < narrow[2]; ++s) {
                    auto& slot = tr.inventory[static_cast<std::size_t>(s)];
                    if (slot.item_no == narrow[1]) {
                        slot.clear();
                        ++removed;
                        value_sum += item_val;
                    }
                }
            };
            try_remove_range(game::kNumTroopEquipmentSlots,
                             game::kNumTroopEquipmentSlots + bag_cap);
            try_remove_range(0, game::kNumTroopEquipmentSlots);
            ctx.registers[0] = value_sum;
        }
        return NativeDispatch::handled;
    case 1537: // troop_inventory_slot_get_item_amount (0x1401d0b64)
        if (world.is_valid_troop(narrow[1])) {
            const auto* slot = native_troop_slot_ptr(
                world.troops[static_cast<std::size_t>(narrow[1])], narrow[2], world);
            if (slot != nullptr) {
                store_lvalue(dst, static_cast<std::int64_t>(slot->ammo), ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1538: // troop_inventory_slot_get_item_max_amount (0x1401d0c20)
        if (world.is_valid_troop(narrow[1])) {
            const auto* slot = native_troop_slot_ptr(
                world.troops[static_cast<std::size_t>(narrow[1])], narrow[2], world);
            if (slot != nullptr) {
                const std::int32_t max_ammo =
                    world.is_valid_item(slot->item_no)
                        ? world.items[static_cast<std::size_t>(slot->item_no)].max_ammo
                        : 0;
                store_lvalue(dst, static_cast<std::int64_t>(max_ammo), ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1539: // troop_loot_troop (0x1401d0cdc)
        if (narrow[2] > 0 && world.is_valid_troop(narrow[0]) &&
            world.is_valid_troop(narrow[1])) {
            auto& dst_tr = world.troops[static_cast<std::size_t>(narrow[0])];
            const auto& src_tr = world.troops[static_cast<std::size_t>(narrow[1])];
            const std::int32_t inv_max =
                world.skills[static_cast<std::size_t>(game::kSkillInventoryManagement)]
                    .max_level;
            std::int32_t next_free = dst_tr.get_first_free_inventory_slot(inv_max);
            const std::int32_t cap_end =
                game::kNumTroopEquipmentSlots + dst_tr.get_num_inventory_slots(inv_max);
            if (next_free >= 0) {
                for (std::int32_t i = 0; i < 50 && next_free < cap_end; ++i) {
                    const auto& item = src_tr.inventory[static_cast<std::size_t>(i)];
                    if (world.is_valid_item(item.item_no) &&
                        ((world.items[static_cast<std::size_t>(item.item_no)].properties >> 13U) &
                         1U) != 0U) {
                        native_loot_copy_slot(dst_tr, next_free++, item);
                    }
                }
                for (std::int32_t tries = 0; tries < 10 && next_free < cap_end; ++tries) {
                    const std::int32_t pick = native_rgl_rand(ctx, 50);
                    const auto& item = src_tr.inventory[static_cast<std::size_t>(pick)];
                    if (!world.is_valid_item(item.item_no)) {
                        continue;
                    }
                    const auto props =
                        world.items[static_cast<std::size_t>(item.item_no)].properties;
                    if (((props >> 12U) & 1U) != 0U || ((props >> 13U) & 1U) != 0U) {
                        continue;
                    }
                    const float val_pow = std::pow(
                        static_cast<float>(
                            world.items[static_cast<std::size_t>(item.item_no)].value),
                        0.8f);
                    const float p_base = std::min(20.0f / (val_pow + 20.0f), 1.0f);
                    if (native_entity_randf(ctx) <
                        p_base * static_cast<float>(wide[2]) * 0.01f) {
                        native_loot_copy_slot(dst_tr, next_free++, item);
                    }
                }
            }
        }
        return NativeDispatch::handled;
    case 1540: // troop_get_inventory_capacity (0x1401d0f25)
        if (world.is_valid_troop(narrow[1])) {
            const std::int32_t cap =
                world.troops[static_cast<std::size_t>(narrow[1])].get_num_inventory_slots(
                    world.skills[static_cast<std::size_t>(game::kSkillInventoryManagement)]
                        .max_level) +
                game::kNumTroopEquipmentSlots;
            store_lvalue(dst, static_cast<std::int64_t>(cap), ctx, locals);
        } else {
            store_lvalue(dst, 0, ctx, locals);
        }
        return NativeDispatch::handled;
    case 1541: { // troop_get_inventory_slot (0x1401d0f8e)
        std::int64_t out = -1;
        if (world.is_valid_troop(narrow[1])) {
            const auto* slot = native_troop_slot_ptr(
                world.troops[static_cast<std::size_t>(narrow[1])], narrow[2], world);
            if (slot != nullptr) {
                out = static_cast<std::int64_t>(slot->item_no);
            }
        }
        store_lvalue(dst, out, ctx, locals);
        return NativeDispatch::handled;
    }
    case 1542: { // troop_get_inventory_slot_modifier (0x1401d107e)
        std::int64_t out = -1;
        if (world.is_valid_troop(narrow[1])) {
            const auto* slot = native_troop_slot_ptr(
                world.troops[static_cast<std::size_t>(narrow[1])], narrow[2], world);
            if (slot != nullptr) {
                out = static_cast<std::int64_t>(
                    (slot->modifier >= 0 && slot->modifier < 0x2b) ? slot->modifier : 0);
            }
        }
        store_lvalue(dst, out, ctx, locals);
        return NativeDispatch::handled;
    }
    case 1550: // troop_set_faction (0x1401d12a0)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_faction(narrow[1])) {
            world.troops[static_cast<std::size_t>(narrow[0])].faction_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 2170: // store_skill_level (0x1401dd8e0)
        // `getTroopSkill(troop, skill, true)` (`0x1400dcbf0`): includes
        // `game_get_skill_modifier_for_troop`.
        if (world.is_valid_troop(narrow[2]) &&
            narrow[1] >= 0 && narrow[1] < game::kMaxSkills) {
            store_lvalue(dst, static_cast<std::int64_t>(troop_skill_level(ctx, narrow[2], narrow[1])),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2171: // store_character_level (0x1401dd92b)
        if (world.is_valid_troop(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.troops[static_cast<std::size_t>(narrow[1])].level),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2172: // store_attribute_level (0x1401dd972)
        if (world.is_valid_troop(narrow[1]) &&
            narrow[2] >= 0 && narrow[2] < game::kMaxAttributes) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.troops[static_cast<std::size_t>(narrow[1])]
                                 .attributes[static_cast<std::size_t>(narrow[2])]),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2175: // store_troop_health (0x1401dda1f)
        if (world.is_valid_troop(narrow[1])) {
            const auto& tr = world.troops[static_cast<std::size_t>(narrow[1])];
            if (wide[2] == 0) {
                const std::int32_t max_hp = std::max<std::int32_t>(
                    1, tr.max_hit_points(
                           world.skills[static_cast<std::size_t>(game::kSkillIronflesh)]
                               .max_level));
                const auto pct = static_cast<std::int32_t>(
                    (tr.hit_points * 100.0f) / static_cast<float>(max_hp));
                store_lvalue(dst, static_cast<std::int64_t>(pct), ctx, locals);
            } else {
                store_lvalue(dst, static_cast<std::int64_t>(static_cast<std::int32_t>(tr.hit_points)),
                             ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 2176: // store_proficiency_level (0x1401ddabf)
        if (world.is_valid_troop(narrow[1]) &&
            narrow[2] >= 0 && narrow[2] < game::kMaxProficiencies) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(static_cast<std::int32_t>(
                             world.troops[static_cast<std::size_t>(narrow[1])]
                                 .proficiencies[static_cast<std::size_t>(narrow[2])])),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    // Parties & Campaign
    case 1601: // party_get_num_companions (0x1401d14a7)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .get_num_companions()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1602: // party_get_num_prisoners (0x1401d14f2)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .get_num_prisoners()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1603: // party_set_flags (0x1401d153d)
        if (world.is_valid_party(narrow[0])) {
            auto& fl = world.parties[static_cast<std::size_t>(narrow[0])].flags;
            const auto mask = static_cast<std::uint64_t>(wide[1]);
            fl = (wide[2] == 0) ? (fl & ~mask) : (fl | mask);
        }
        return NativeDispatch::handled;
    case 1608: // party_get_current_terrain (0x1401d171e)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .current_terrain),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1610: // party_add_members (0x1401d17ac)
        if (world.is_valid_party(narrow[0]) && world.is_valid_troop(narrow[1]) &&
            narrow[2] > 0) {
            ctx.registers[0] = static_cast<std::int64_t>(
                world.parties[static_cast<std::size_t>(narrow[0])]
                    .add_members(narrow[1], narrow[2], false));
        }
        return NativeDispatch::handled;
    case 1615: // party_remove_members (0x1401d1986)
        ctx.registers[0] = 0;
        if (world.is_valid_party(narrow[0]) && world.is_valid_troop(narrow[1]) &&
            narrow[2] > 0) {
            ctx.registers[0] = static_cast<std::int64_t>(
                world.parties[static_cast<std::size_t>(narrow[0])]
                    .remove_members(narrow[1], narrow[2], false));
        }
        return NativeDispatch::handled;
    case 1620: // party_set_faction (0x1401d1c85)
        if (world.is_valid_party(narrow[0]) && world.is_valid_faction(narrow[1])) {
            world.parties[static_cast<std::size_t>(narrow[0])].faction_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1625: // party_get_position (0x1401d1da8)
        if (wse_register_ok(narrow[0]) && world.is_valid_party(narrow[1])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.parties[static_cast<std::size_t>(narrow[1])].position;
        }
        return NativeDispatch::handled;
    case 1626: // party_set_position (0x1401d1e51)
        if (world.is_valid_party(narrow[0]) && wse_register_ok(narrow[1])) {
            world.parties[static_cast<std::size_t>(narrow[0])].position =
                ctx.positions[static_cast<std::size_t>(narrow[1])];
        }
        return NativeDispatch::handled;
    case 1640: // party_set_ai_behavior (0x1401d2317)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].behavior = narrow[1];
        }
        return NativeDispatch::handled;
    case 1641: // party_set_ai_object (0x1401d2362)
        if (world.is_valid_party(narrow[0]) &&
            (narrow[1] == -1 || world.is_valid_party(narrow[1]))) {
            world.parties[static_cast<std::size_t>(narrow[0])].behavior_object = narrow[1];
        }
        return NativeDispatch::handled;
    case 1650: // party_get_num_companion_stacks (0x1401d25e9)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .get_num_companion_stacks()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1651: // party_get_num_prisoner_stacks (0x1401d2634)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .get_num_prisoner_stacks()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1652: // party_stack_get_troop_id (0x1401d267f)
        if (world.is_valid_party(narrow[1])) {
            const auto& st = world.parties[static_cast<std::size_t>(narrow[1])].stacks;
            if (narrow[2] >= 0 && static_cast<std::size_t>(narrow[2]) < st.size()) {
                store_lvalue(dst,
                             static_cast<std::int64_t>(
                                 st[static_cast<std::size_t>(narrow[2])].troop_no),
                             ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1653: // party_stack_get_size (0x1401d26ec)
        if (world.is_valid_party(narrow[1])) {
            const auto& st = world.parties[static_cast<std::size_t>(narrow[1])].stacks;
            if (narrow[2] >= 0 && static_cast<std::size_t>(narrow[2]) < st.size()) {
                store_lvalue(dst,
                             static_cast<std::int64_t>(
                                 st[static_cast<std::size_t>(narrow[2])].num_troops),
                             ctx, locals);
            }
        }
        return NativeDispatch::handled;
    case 1660: // party_attach_to_party (0x1401d2a10)
        (void)world.attach_party(narrow[0], narrow[1]);
        return NativeDispatch::handled;
    case 1661: // party_detach (0x1401d2a62)
        (void)world.detach_party(narrow[0]);
        return NativeDispatch::handled;
    case 1665: // party_get_cur_town (0x1401d2afa)
        if (world.is_valid_party(narrow[1])) {
            const auto& p = world.parties[static_cast<std::size_t>(narrow[1])];
            store_lvalue(dst,
                         static_cast<std::int64_t>(p.behavior == 8 ? p.behavior_object : -1),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1669: { // party_set_name (0x1401d2be7)
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (world.is_valid_party(narrow[0]) && wse_string_register_ok(narrow[1], tag1, banks)) {
            world.parties[static_cast<std::size_t>(narrow[0])].name =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    }
    case 1676: // party_set_icon (0x1401d2f70)
        if (world.is_valid_party(narrow[0]) && narrow[1] >= 0) {
            world.parties[static_cast<std::size_t>(narrow[0])].icon_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1677: // party_set_banner_icon (0x1401d3022)
        if (world.is_valid_party(narrow[0]) && narrow[1] >= 0) {
            world.parties[static_cast<std::size_t>(narrow[0])].banner_icon_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 1680: // party_get_battle_opponent (0x1401d30de)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .battle_opponent_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1681: // party_get_icon (0x1401d31d6)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])].icon_no),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1685: // party_get_skill_level (0x1401d3305)
        if (world.is_valid_party(narrow[1]) && narrow[2] >= 0 &&
            narrow[2] < game::kMaxSkills) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .get_skill(narrow[2], world.troops, world.skills)),
                         ctx, locals);
        } else {
            store_lvalue(dst, 0, ctx, locals);
        }
        return NativeDispatch::handled;

    case 2156: // store_party_size (0x1401dd642)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])].get_size()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2157: // store_party_size_wo_prisoners (0x1401dd68d)
        if (world.is_valid_party(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.parties[static_cast<std::size_t>(narrow[1])]
                                 .get_num_companions()),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2158: { // store_troop_kind_count (0x1401dd6d8)
        std::int32_t count = 0;
        if (world.is_valid_party(world.main_party_no)) {
            count = world.parties[static_cast<std::size_t>(world.main_party_no)]
                        .get_troop_kind_count(narrow[1]);
        }
        store_lvalue(dst, static_cast<std::int64_t>(count), ctx, locals);
        return NativeDispatch::handled;
    }
    case 2201: { // store_partner_faction (0x1401ddcea)
        std::int32_t fac = world.partner_faction_no;
        if (world.is_valid_troop(world.conversation_partner_troop_no)) {
            fac = world.troops[static_cast<std::size_t>(world.conversation_partner_troop_no)]
                      .faction_no;
        }
        store_lvalue(dst, static_cast<std::int64_t>(fac), ctx, locals);
        return NativeDispatch::handled;
    }
    case 2270: // store_current_hours (0x1401de26d)
        store_lvalue(dst, static_cast<std::int64_t>(world.current_hours), ctx, locals);
        return NativeDispatch::handled;
    case 2281: // store_distance_to_party_from_party (0x1401de32d)
        if (world.is_valid_party(narrow[1]) && world.is_valid_party(narrow[2])) {
            const auto& p1 = world.parties[static_cast<std::size_t>(narrow[1])].position.o;
            const auto& p2 = world.parties[static_cast<std::size_t>(narrow[2])].position.o;
            store_lvalue(dst, f2i_round((p1 - p2).length()), ctx, locals);
        }
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_mission_statement(opcode, wide, narrow, tags, statement, ctx, locals);
}

}  // namespace wse::vm

#include "wse/vm/native_mission_ops.h"
