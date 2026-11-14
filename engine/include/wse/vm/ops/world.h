#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline std::int32_t wse::game::WseTroop::*
wse_troop_points_member(std::uint32_t opcode) noexcept {
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::troop_get_skill_points:
    case WseOpcode::troop_set_skill_points:
        return &wse::game::WseTroop::skill_points;
    case WseOpcode::troop_get_attribute_points:
    case WseOpcode::troop_set_attribute_points:
        return &wse::game::WseTroop::attribute_points;
    case WseOpcode::troop_get_proficiency_points:
    case WseOpcode::troop_set_proficiency_points:
        return &wse::game::WseTroop::proficiency_points;
    default:
        return nullptr;
    }
}

// Mirrors 3800..3803 (0x1401ebf9f..0x1401ec0cf): low byte vs bits 8..9.
inline void wse_set_item_damage_part(wse::game::WseItemKind& item,
                                     std::uint32_t opcode,
                                     std::int32_t value) noexcept {
    const auto op = static_cast<WseOpcode>(opcode);
    std::int32_t& target =
        (op == WseOpcode::item_set_thrust_damage ||
         op == WseOpcode::item_set_thrust_damage_type)
            ? item.thrust_damage
            : item.swing_damage;
    const auto bits = static_cast<std::uint32_t>(target);
    const auto uval = static_cast<std::uint32_t>(value);
    if (op == WseOpcode::item_set_thrust_damage ||
        op == WseOpcode::item_set_swing_damage) {
        target = static_cast<std::int32_t>((bits & ~0xffu) | (uval & 0xffu));
    } else {
        target = static_cast<std::int32_t>((bits & 0xffu) | ((uval & 3u) << 8));
    }
}

[[nodiscard]] inline std::int32_t wse::game::WseItemKind::*
wse_item_int_member(std::uint32_t opcode) noexcept {
    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::item_set_head_armor:
        return &wse::game::WseItemKind::head_armor;
    case WseOpcode::item_set_body_armor:
        return &wse::game::WseItemKind::body_armor;
    case WseOpcode::item_set_leg_armor:
        return &wse::game::WseItemKind::leg_armor;
    case WseOpcode::item_set_speed_rating:
        return &wse::game::WseItemKind::speed_rating;
    case WseOpcode::item_set_missile_speed:
        return &wse::game::WseItemKind::missile_speed;
    default:
        return nullptr;
    }
}

[[nodiscard]] inline std::int32_t wse_edit_mode_find_selected(
    const wse::game::WseEditModeState& edit,
    std::int32_t instance_no) noexcept {
    for (std::size_t i = 0; i < edit.selected_prop_instances.size(); ++i) {
        if (edit.selected_prop_instances[i] == instance_no) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

[[nodiscard]] inline WseDispatch execute_wse_world(std::uint32_t opcode,
                                                     const WseOperands& operands,
                                                     WseEffect& effect) {
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    const auto tags = operands.tags;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::store_cur_mission_template_no: {
        // 3600 (0x1401ea6b2): null g_currentMission (0x14061f078) yields -1,
        // otherwise the int32 at +0x9ae8 (cur_mission_template_no).
        effect.value = operands.state.current_mission_template_no
                           ? *operands.state.current_mission_template_no
                           : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::get_campaign_time: {
        // 3037 (0xbdd, 0x1401e63f0): reads DAT_14061f038.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = world->campaign_time;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::set_campaign_time: {
        // 3038 (0xbde, 0x1401e63fc): writes DAT_14061f038 from wide[0].
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->campaign_time = wide[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::show_hud: {
        // 3045 (0xbe5, L1571): DAT_140657110 = (*param_3 == 0).
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->hide_hud = (wide[0] == 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::set_show_use_tooltip: {
        // 3601 (0xe11, 0x1401ea71d): 0 <= type < 11 writes mbMission +0x54a40.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || narrow[0] < 0 ||
            narrow[0] >= wse::game::kNumUseTooltipTypes) {
            return WseDispatch::ignored;
        }
        world->use_objects_tooltip_enabled[static_cast<std::size_t>(
            narrow[0])] = (narrow.size() >= 2 && narrow[1] != 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::stop_time: {
        // 3608 (0xe18, 0x1401ea9da): requires cheat_mode (0x14061ef50).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            operands.state.options == nullptr ||
            !operands.state.options->cheat_mode) {
            return WseDispatch::ignored;
        }
        world->paused = (narrow[0] != 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_water_level: {
        // 3610 (0xe1a, 0x1401eabd6): rglRound64(fixed_point * water_level).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(
            static_cast<float>(operands.state.fixed_point_multiplier) *
            world->water_level);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::troop_get_skill_points:
    case WseOpcode::troop_get_attribute_points:
    case WseOpcode::troop_get_proficiency_points: {
        // 3700/3702/3704: read mbTroop +0x18c/+0x190/+0x194.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        const auto member = wse_troop_points_member(opcode);
        if (world == nullptr || member == nullptr ||
            !world->is_valid_troop(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->troops[static_cast<std::size_t>(narrow[1])].*member;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::troop_set_skill_points:
    case WseOpcode::troop_set_attribute_points:
    case WseOpcode::troop_set_proficiency_points: {
        // 3701/3703/3705: write non-negative points into mbTroop.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        const auto member = wse_troop_points_member(opcode);
        if (world == nullptr || member == nullptr ||
            !world->is_valid_troop(narrow[0]) || narrow[1] < 0) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].*member = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::troop_has_flag: {
        // 3706 (0xe7a, L4431): invalid troop logs and returns ignored; else
        // tests (mbTroop::m_flags & wide[1]) != 0.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0])) {
            return WseDispatch::ignored;
        }
        const auto flags =
            world->troops[static_cast<std::size_t>(narrow[0])].flags;
        return (flags & static_cast<std::uint64_t>(wide[1])) != 0
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::troop_set_skill: {
        // 3707 (0xe7b, L4438 -> 0x1402460b0): clamps to skill m_maxLevel,
        // adjusts HP for Ironflesh increase, and applies shared HP cap.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= wse::game::kMaxSkills) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].set_skill(
            narrow[1], narrow[2],
            world->skills[static_cast<std::size_t>(narrow[1])].max_level,
            world->skills[static_cast<std::size_t>(wse::game::kSkillIronflesh)]
                .max_level);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::troop_set_attribute: {
        // 3708 (0xe7c, L4451 -> 0x140246010): clamps [0, INT_MAX] and caps HP.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= wse::game::kMaxAttributes) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].set_attribute(
            narrow[1], narrow[2],
            world->skills[static_cast<std::size_t>(wse::game::kSkillIronflesh)]
                .max_level);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::troop_set_proficiency: {
        // 3709 (0xe7d, L4464 -> 0x1400a61c0): clamps [0, 999] into float.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_troop(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= wse::game::kMaxProficiencies) {
            return WseDispatch::ignored;
        }
        world->troops[static_cast<std::size_t>(narrow[0])].set_proficiency(
            narrow[1], narrow[2]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_thrust_damage:
    case WseOpcode::item_set_thrust_damage_type:
    case WseOpcode::item_set_swing_damage:
    case WseOpcode::item_set_swing_damage_type: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0])) {
            return WseDispatch::ignored;
        }
        wse_set_item_damage_part(
            world->items[static_cast<std::size_t>(narrow[0])], opcode,
            narrow[1]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_head_armor:
    case WseOpcode::item_set_body_armor:
    case WseOpcode::item_set_leg_armor:
    case WseOpcode::item_set_speed_rating:
    case WseOpcode::item_set_missile_speed: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        const auto member = wse_item_int_member(opcode);
        if (world == nullptr || member == nullptr ||
            !world->is_valid_item(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->items[static_cast<std::size_t>(narrow[0])].*member = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_horse_blood_particles: {
        // 3809 (0xee1, L4555): requires valid item and two particle systems.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0]) ||
            !world->is_valid_particle_system(narrow[1]) ||
            !world->is_valid_particle_system(narrow[2])) {
            return WseDispatch::ignored;
        }
        world->items[static_cast<std::size_t>(narrow[0])]
            .blood_particle_system_nos = {narrow[1], narrow[2]};
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_horse_blood_color: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->items[static_cast<std::size_t>(narrow[0])].blood_color =
            static_cast<std::uint32_t>(narrow[1]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::cur_item_mesh_set_color: {
        // 3811 (0xee3, L4579): caller == -50, active rglMetaMesh (0x1401c5e40).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (operands.state.caller != -50 || world == nullptr ||
            world->cur_item_meta_mesh == nullptr) {
            return WseDispatch::ignored;
        }
        world->cur_item_meta_mesh->set_mesh_color(
            narrow[0], static_cast<std::uint32_t>(narrow[1]));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::cur_item_add_mesh_with_material: {
        // 3812 (0xee4, L4602): parses mesh & material, converts spaces to
        // underscores (0x14030f270), clones meshes per LOD [begin, end).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_string_register_ok(narrow[0], wse_tag_at(tags, 0), strings) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings)) {
            return WseDispatch::ignored;
        }
        auto* const world = operands.state.world;
        if (operands.state.caller != -50 || world == nullptr ||
            world->cur_item_meta_mesh == nullptr) {
            return WseDispatch::ignored;
        }
        std::string mesh_name;
        std::string material_name;
        if (!wse_resolve_parsed_utf8_operand(0, operands, mesh_name) ||
            !wse_resolve_parsed_utf8_operand(1, operands, material_name)) {
            return WseDispatch::ignored;
        }
        wse_spaces_to_underscores(mesh_name);
        wse_spaces_to_underscores(material_name);
        const auto mesh_it = world->resource_meshes.find(mesh_name);
        if (mesh_it == world->resource_meshes.end()) {
            return WseDispatch::handled_no_store;
        }
        const auto mat_it = world->resource_materials.find(material_name);
        const bool mat_found =
            mat_it != world->resource_materials.end() && mat_it->second;
        auto& meta = *world->cur_item_meta_mesh;
        const std::int32_t lod_begin = narrow.size() >= 3 ? narrow[2] : 0;
        std::int32_t lod_end = narrow.size() >= 4 ? narrow[3] : 0;
        if (lod_end == 0) {
            lod_end = static_cast<std::int32_t>(meta.lods.size());
        }
        const std::int32_t color = narrow.size() >= 5 ? narrow[4] : 0;
        if (lod_begin >= 0) {
            for (std::int32_t lod = lod_begin;
                 lod < lod_end &&
                 static_cast<std::size_t>(lod) < mesh_it->second.size() &&
                 static_cast<std::size_t>(lod) <
                     wse::game::WseMetaMesh::kMaxLods;
                 ++lod) {
                wse::game::WseMesh clone =
                    mesh_it->second[static_cast<std::size_t>(lod)];
                clone.set_material(mat_found ? std::string_view{material_name}
                                             : std::string_view{});
                if (color > 0) {
                    clone.set_color(static_cast<std::uint32_t>(color));
                }
                const auto u_lod = static_cast<std::size_t>(lod);
                if (meta.lods.size() <= u_lod) {
                    meta.lods.resize(u_lod + 1);
                }
                meta.lods[u_lod].push_back(std::move(clone));
            }
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::item_set_horse_skeleton_model: {
        // 3813 (0xee5, L4645): parses skeleton name and updates m_skeletonModel
        // only when resolved in resource registry (0x1402f2150).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_item(narrow[0]) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings)) {
            return WseDispatch::ignored;
        }
        std::string skel_name;
        if (!wse_resolve_parsed_utf8_operand(1, operands, skel_name)) {
            return WseDispatch::ignored;
        }
        if (const auto it = world->resource_skeleton_models.find(skel_name);
            it != world->resource_skeleton_models.end() && it->second) {
            world->items[static_cast<std::size_t>(narrow[0])].skeleton_model =
                std::move(skel_name);
        }
        return WseDispatch::handled_no_store;
    }
    // Edit mode group (4600..4605, 4607..4609; 4606 shares wse_option_flag).
    case WseOpcode::edit_mode_get_num_selected_prop_instances: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = (world != nullptr && world->has_game &&
                        world->edit_mode.active)
                           ? static_cast<std::int64_t>(
                                 world->edit_mode.selected_prop_instances.size())
                           : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_get_selected_prop_instance: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = -1;
        if (world != nullptr && world->has_game && world->edit_mode.active &&
            narrow[1] >= 0 &&
            static_cast<std::size_t>(narrow[1]) <
                world->edit_mode.selected_prop_instances.size()) {
            effect.value =
                world->edit_mode
                    .selected_prop_instances[static_cast<std::size_t>(narrow[1])];
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_select_prop_instance: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            !world->is_valid_mission_object(narrow[0]) ||
            !world->edit_mode.active) {
            return WseDispatch::ignored;
        }
        if (wse_edit_mode_find_selected(world->edit_mode, narrow[0]) < 0) {
            world->edit_mode.selected_prop_instances.push_back(narrow[0]);
            world->edit_mode.selection_changed = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::edit_mode_deselect_prop_instance: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            !world->is_valid_mission_object(narrow[0]) ||
            !world->edit_mode.active) {
            return WseDispatch::ignored;
        }
        const std::int32_t idx =
            wse_edit_mode_find_selected(world->edit_mode, narrow[0]);
        if (idx >= 0) {
            if (world->edit_mode.highlighted_index == idx) {
                world->edit_mode.highlighted_index = -1;
            } else if (world->edit_mode.highlighted_index > idx) {
                --world->edit_mode.highlighted_index;
            }
            world->edit_mode.selected_prop_instances.erase(
                world->edit_mode.selected_prop_instances.begin() + idx);
            world->edit_mode.selection_changed = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::edit_mode_get_highlighted_prop_instance: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = -1;
        if (world != nullptr && world->has_game && world->edit_mode.active &&
            world->edit_mode.highlighted_index >= 0 &&
            static_cast<std::size_t>(world->edit_mode.highlighted_index) <
                world->edit_mode.selected_prop_instances.size()) {
            effect.value =
                world->edit_mode.selected_prop_instances
                    [static_cast<std::size_t>(world->edit_mode.highlighted_index)];
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_set_highlighted_prop_instance: {
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game || !world->edit_mode.active) {
            return WseDispatch::ignored;
        }
        const std::int32_t target = narrow.empty() ? -1 : narrow[0];
        const std::int32_t idx =
            wse_edit_mode_find_selected(world->edit_mode, target);
        if (world->edit_mode.highlighted_index != idx) {
            world->edit_mode.highlighted_index = idx;
            world->edit_mode.selection_changed = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::edit_mode_in_edit_objects_mode: {
        const auto* const world = operands.state.world;
        return (world != nullptr && world->has_game && world->edit_mode.active &&
                world->edit_mode.mode == 0)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::edit_mode_get_mode: {
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        effect.value = (world != nullptr && world->has_game)
                           ? world->edit_mode.mode
                           : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::edit_mode_window_open: {
        const auto* const world = operands.state.world;
        return (world != nullptr && world->has_game &&
                world->edit_mode.dialog_open)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    // Agent action speed modifier (3340..3341).
    case WseOpcode::agent_set_action_speed_modifier: {
        // 3340 (0xd0c, L2863): requires active game, valid agent, [0, 1000]
        // inclusive; stores (double)wide[1] / 100.0f on mbAgent.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            !world->is_valid_agent(narrow[0]) || narrow[1] < 0 ||
            narrow[1] > 1000) {
            return WseDispatch::ignored;
        }
        world->agents[static_cast<std::size_t>(narrow[0])]
            .action_speed_modifier = static_cast<float>(wide[1]) / 100.0f;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::agent_get_action_speed_modifier: {
        // 3341 (0xd0d, L2876): rounds m_actionSpeedModifier * 100.0f.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->has_game ||
            !world->is_valid_agent(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value = static_cast<std::int64_t>(
            std::llround(world->agents[static_cast<std::size_t>(narrow[1])]
                             .action_speed_modifier *
                         100.0f));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    // Party extensions (3900..3911).
    case WseOpcode::party_stack_get_experience: {
        // 3900 (0xf3c, L3687): rounds mbPartyStack::m_xp.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& party = world->parties[static_cast<std::size_t>(narrow[1])];
        if (narrow[2] < 0 ||
            narrow[2] >= static_cast<std::int32_t>(party.stacks.size())) {
            return WseDispatch::ignored;
        }
        effect.value = static_cast<std::int64_t>(
            std::llround(party.stacks[static_cast<std::size_t>(narrow[2])].xp));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::party_stack_get_num_upgradeable: {
        // 3901 (0xf3d, L4701): stores mbPartyStack::m_numUpgradeable.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& party = world->parties[static_cast<std::size_t>(narrow[1])];
        if (narrow[2] < 0 ||
            narrow[2] >= static_cast<std::int32_t>(party.stacks.size())) {
            return WseDispatch::ignored;
        }
        effect.value =
            party.stacks[static_cast<std::size_t>(narrow[2])].num_upgradeable;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::party_has_flag: {
        // 3902 (0xf3e, L4715): tests (mbParty::m_flags & wide[1]) != 0.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0])) {
            return WseDispatch::ignored;
        }
        const auto flags =
            world->parties[static_cast<std::size_t>(narrow[0])].flags;
        return (flags & static_cast<std::uint64_t>(wide[1])) != 0
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::party_heal_members: {
        // 3903 (0xf3f, L4722): heals up to <number> matching wounded troops.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0]) ||
            !world->is_valid_troop(narrow[1])) {
            return WseDispatch::ignored;
        }
        wse::game::party_heal_members(
            world->parties[static_cast<std::size_t>(narrow[0])], narrow[1],
            narrow[2]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::party_switch_stacks: {
        // 3904 (0xf40, L4782): swaps two stacks if both are in bounds.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0])) {
            return WseDispatch::ignored;
        }
        auto& party = world->parties[static_cast<std::size_t>(narrow[0])];
        const auto count = static_cast<std::int32_t>(party.stacks.size());
        if (narrow[1] < 0 || narrow[1] >= count || narrow[2] < 0 ||
            narrow[2] >= count) {
            return WseDispatch::ignored;
        }
        wse::game::party_switch_stacks(party, narrow[1], narrow[2]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::party_stack_upgrade: {
        // 3905 (0xf41, L4827): requires amount > 0, valid stack & target.
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0]) ||
            narrow[2] <= 0) {
            return WseDispatch::ignored;
        }
        auto& party = world->parties[static_cast<std::size_t>(narrow[0])];
        if (narrow[1] < 0 ||
            narrow[1] >= static_cast<std::int32_t>(party.stacks.size())) {
            return WseDispatch::ignored;
        }
        const auto& stack = party.stacks[static_cast<std::size_t>(narrow[1])];
        if (!world->is_valid_troop(stack.troop_no)) {
            return WseDispatch::ignored;
        }
        const std::size_t path = (narrow[3] != 0) ? 1u : 0u;
        const std::int32_t target_troop =
            world->troops[static_cast<std::size_t>(stack.troop_no)]
                .upgrade_troop_nos[path];
        if (!world->is_valid_troop(target_troop)) {
            return WseDispatch::ignored;
        }
        wse::game::party_stack_upgrade(*world, narrow[0], narrow[1], narrow[2],
                                       static_cast<std::int32_t>(path));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::party_stack_set_num_upgradeable: {
        // 3906 (0xf42, L4974): stores raw value into m_numUpgradeable.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0])) {
            return WseDispatch::ignored;
        }
        auto& party = world->parties[static_cast<std::size_t>(narrow[0])];
        if (narrow[1] < 0 ||
            narrow[1] >= static_cast<std::int32_t>(party.stacks.size())) {
            return WseDispatch::ignored;
        }
        party.stacks[static_cast<std::size_t>(narrow[1])].num_upgradeable =
            narrow[2];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::party_get_banner_icon: {
        // 3907 (0xf43, L4988): stores mbParty::m_bannerMapIconNo.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->parties[static_cast<std::size_t>(narrow[1])].banner_icon_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::party_get_extra_icon: {
        // 3908 (0xf44, L4996): stores mbParty::m_extraMapIconNo.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->parties[static_cast<std::size_t>(narrow[1])].extra_icon_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::party_get_player_id: {
        // 3909 (0xf45, L5004): stores mbParty::m_playerNo.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->parties[static_cast<std::size_t>(narrow[1])].player_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::party_is_non_player: {
        // 3910 (0xf46, L5012): succeeds iff mbParty::isBot() is true.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[0])) {
            return WseDispatch::ignored;
        }
        return world->parties[static_cast<std::size_t>(narrow[0])].is_bot()
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::party_get_current_terrain_variant: {
        // 3911 (0xf47, L5020): stores world map terrain texture variant.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (world == nullptr || !world->is_valid_party(narrow[1])) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->parties[static_cast<std::size_t>(narrow[1])].terrain_variant;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
