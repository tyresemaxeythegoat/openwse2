// Milestone 6 UI, menu, overlay, presentation, input, URL, resource, and
// skin/skeleton opcode handlers (33 WSE opcodes: 3018, 3019, 3023..3025, 3029,
// 3032, 3033, 3039, 3044, 3100..3102, 4229, 4700..4703, 4800..4803, 4900..4907,
// 5200..5202).

#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "wse/core/math.h"
#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline bool wse_m6_is_allowed_shell_url(
    std::string_view url) noexcept {
    constexpr std::array<std::string_view, 4> kPrefixes = {
        "http://",
        "https://",
        "ftp://",
        "ts3server://",
    };
    for (const auto prefix : kPrefixes) {
        if (url.size() < prefix.size()) {
            continue;
        }
        bool match = true;
        for (std::size_t i = 0; i < prefix.size(); ++i) {
            const auto ch = static_cast<unsigned char>(url[i]);
            if (static_cast<char>(std::tolower(ch)) != prefix[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline WseDispatch wse_m6_enqueue_url_request(
    const WseOperands& operands,
    bool is_post) {
    const auto narrow = operands.narrow;
    const std::size_t base_idx = is_post ? 3U : 2U;
    if (narrow.size() < base_idx) {
        return WseDispatch::operand_count_out_of_range;
    }
    auto* world = operands.state.world;
    std::string url;
    std::string user_agent;
    std::string post_data;
    if (world == nullptr || !wse_resolve_parsed_utf8_operand(0, operands, url) ||
        !wse_resolve_parsed_utf8_operand(1, operands, user_agent) ||
        (is_post && !wse_resolve_parsed_utf8_operand(2, operands, post_data))) {
        return WseDispatch::ignored;
    }

    wse::game::WseM6UrlRequest req{};
    req.url = url;
    req.user_agent = std::move(user_agent);
    req.post_data = std::move(post_data);
    req.is_post = is_post;
    req.success_script_no =
        (narrow.size() > base_idx) ? narrow[base_idx] : -1;
    req.failure_script_no =
        (narrow.size() > base_idx + 1U) ? narrow[base_idx + 1U] : -1;
    req.skip_parsing =
        (narrow.size() > base_idx + 2U && narrow[base_idx + 2U] != 0);
    // Safety deviation: the seconds -> milliseconds product saturates instead
    // of overflowing a 32-bit signed multiply.
    req.timeout_ms =
        (narrow.size() > base_idx + 3U && narrow[base_idx + 3U] != 0)
            ? static_cast<std::int32_t>(std::clamp<std::int64_t>(
                  static_cast<std::int64_t>(narrow[base_idx + 3U]) * 1000,
                  std::numeric_limits<std::int32_t>::min(),
                  std::numeric_limits<std::int32_t>::max()))
            : -1;

    world->m6.ui.url_advanced_requests.push_back(std::move(url));
    world->m6.ui.url_advanced_queue.push_back(std::move(req));
    return WseDispatch::handled_no_store;
}

[[nodiscard]] inline WseDispatch wse_m6_reload_resource_file(
    const WseOperands& operands,
    bool& requested_flag,
    std::string& file_slot) {
    if (!operands.narrow.empty() && operands.narrow[0] != 0 &&
        operands.narrow.size() >= 2U) {
        std::string file_name;
        if (wse_resolve_parsed_utf8_operand(1, operands, file_name)) {
            file_slot = std::move(file_name);
        }
    }
    requested_flag = true;
    return WseDispatch::handled_no_store;
}

[[nodiscard]] inline WseDispatch execute_wse_ui_menu(
    std::uint32_t opcode,
    const WseOperands& operands,
    WseEffect& effect) {
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    auto* world = operands.state.world;
    auto strings = operands.state.strings;
    const auto positions = operands.state.positions;
    const float fp_mul =
        static_cast<float>(operands.state.fixed_point_multiplier != 0
                               ? operands.state.fixed_point_multiplier
                               : 100);

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::set_forced_lod: {
        // 3018 (0x1401e5b52)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 || narrow[0] >= 6) {
            return WseDispatch::ignored;
        }
        world->m6.scene.trigger_forced_lod = narrow[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::send_message_to_url_advanced:
        // 3019 (0x1401e5b92)
        return wse_m6_enqueue_url_request(operands, false);
    case WseOpcode::send_post_message_to_url_advanced:
        // 3033 (0x1401e636d)
        return wse_m6_enqueue_url_request(operands, true);
    case WseOpcode::order_flag_is_active: {
        // 3023 (0x1401e5d6d)
        return (world != nullptr && world->m6.ui.order_flag_active)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::play_bink_file: {
        // 3024 (0x1401e5dba)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string path;
        if (world == nullptr ||
            !wse_resolve_parsed_utf8_operand(0, operands, path)) {
            return WseDispatch::ignored;
        }
        const std::int32_t duration_ms =
            (narrow.size() >= 2 && narrow[1] != 0) ? narrow[1] : -1;
        world->m6.ui.bink_videos.emplace_back(std::move(path), duration_ms);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::process_advanced_url_messages: {
        // 3025 (0x1401e5f0b)
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        for (auto& req : world->m6.ui.url_advanced_queue) {
            req.processed = true;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::shell_open_url: {
        // 3029 (0x1401e5fc1)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string url;
        if (world == nullptr ||
            !wse_resolve_parsed_utf8_operand(0, operands, url) ||
            !wse_m6_is_allowed_shell_url(url)) {
            return WseDispatch::ignored;
        }
        world->m6.ui.shell_opened_urls.push_back(std::move(url));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::make_screenshot: {
        // 3032 (0x1401e6221)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file;
        if (world == nullptr || narrow[0] < 0 || narrow[0] >= 4 ||
            !wse_resolve_parsed_utf8_operand(1, operands, file) ||
            !wse_check_path(file)) {
            return WseDispatch::ignored;
        }
        world->m6.ui.screenshots.emplace_back(narrow[0], std::move(file));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_mouse_map_coordinates: {
        // 3039 (0x1401e6643)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= positions.size() ||
            !world->m6.ui.cursor_ray_valid ||
            (world->m6.ui.active_window_id != -1 &&
             world->m6.ui.active_window_id != 6)) {
            return WseDispatch::ignored;
        }
        const WseVec4& orig = world->m6.ui.cursor_ray_origin;
        const WseVec4& dir = world->m6.ui.cursor_ray_dir;
        if (std::fabs(dir.z) <= 1.0e-6f) {
            return WseDispatch::ignored;
        }
        const float t = (world->ground_level_z - orig.z) / dir.z;
        if (t < 0.0f) {
            return WseDispatch::ignored;
        }
        auto& out = positions[static_cast<std::size_t>(narrow[0])].o;
        out.x = orig.x + dir.x * t;
        out.y = orig.y + dir.y * t;
        out.z = world->ground_level_z;
        out.w = 1.0f;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::conversation_screen_auto_update_choice: {
        // 3044 (0x1401e6b7e)
        if (world == nullptr ||
            (world->m6.ui.active_window_id != -1 &&
             world->m6.ui.active_window_id != 12)) {
            return WseDispatch::ignored;
        }
        world->m6.ui.conversation_auto_update_choice = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::game_key_get_key: {
        // 3100 (0x1401e6bc2)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t gk = narrow[1];
        const std::int32_t alt = (narrow.size() >= 3) ? narrow[2] : 0;
        const std::int32_t mod = (narrow.size() >= 4) ? narrow[3] : 0;
        if (world == nullptr || gk < 0 || gk >= 95 || alt < 0 || alt >= 2 ||
            mod < 0 || mod >= 3) {
            return WseDispatch::ignored;
        }
        effect.value =
            world->m6.ui.game_key_mappings[static_cast<std::size_t>(gk)]
                                          [static_cast<std::size_t>(alt)]
                                          [static_cast<std::size_t>(mod)];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::key_released: {
        // 3101 (0x1401e6c14)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 || narrow[0] >= 256) {
            return WseDispatch::ignored;
        }
        const bool bypass = (narrow.size() >= 2 && narrow[1] != 0);
        const auto idx = static_cast<std::size_t>(narrow[0]);
        const bool ok = (bypass || !world->m6.ui.console_open) &&
                        !world->input.key_down[idx] &&
                        world->m6.ui.prev_key_down[idx];
        return ok ? WseDispatch::handled_no_store : WseDispatch::condition_false;
    }
    case WseOpcode::game_key_released: {
        // 3102 (0x1401e6c65)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 || narrow[0] >= 95) {
            return WseDispatch::ignored;
        }
        const bool bypass = (narrow.size() >= 2 && narrow[1] != 0);
        const auto idx = static_cast<std::size_t>(narrow[0]);
        const bool ok = (bypass || !world->m6.ui.console_open) &&
                        !world->input.game_key_down[idx] &&
                        world->m6.ui.prev_game_key_down[idx];
        return ok ? WseDispatch::handled_no_store : WseDispatch::condition_false;
    }
    case WseOpcode::str_store_overlay_text: {
        // 4229 (0x1401ef31a)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !wse_register_ok(narrow[0]) ||
            !world->is_valid_overlay(narrow[1])) {
            return WseDispatch::ignored;
        }
        return wse_store_string(
                   strings, narrow[0],
                   world->overlays[static_cast<std::size_t>(narrow[1])].text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::update_material: {
        // 4700
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string mat;
        std::string new_mat;
        if (world == nullptr ||
            !wse_resolve_parsed_utf8_operand(0, operands, mat) ||
            !wse_resolve_parsed_utf8_operand(1, operands, new_mat)) {
            return WseDispatch::ignored;
        }
        std::replace(mat.begin(), mat.end(), ' ', '_');
        std::replace(new_mat.begin(), new_mat.end(), ' ', '_');
        if (!world->resource_materials.empty() &&
            (!world->resource_materials.contains(mat) ||
             !world->resource_materials.contains(new_mat))) {
            return WseDispatch::ignored;
        }
        world->m6.ui.updated_materials.push_back(mat + "->" + new_mat);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::reload_item_kinds: {
        // 4701 (0x1401f1ff8)
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        return wse_m6_reload_resource_file(
            operands, world->m6.ui.reload_item_kinds_requested,
            world->m6.ui.item_kinds_file);
    }
    case WseOpcode::reload_troops: {
        // 4702 (0x1401f206a)
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        return wse_m6_reload_resource_file(
            operands, world->m6.ui.reload_troops_requested,
            world->m6.ui.troops_file);
    }
    case WseOpcode::reload_parties: {
        // 4703
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        return wse_m6_reload_resource_file(
            operands, world->m6.ui.reload_parties_requested,
            world->m6.ui.parties_file);
    }
    case WseOpcode::menu_create_new: {
        // 4800 (0x1401f214e)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        std::string text;
        std::string mesh_name;
        if (!wse_resolve_parsed_utf8_operand(1, operands, text) ||
            (narrow.size() >= 3 &&
             !wse_resolve_parsed_utf8_operand(2, operands, mesh_name))) {
            effect.value = -1;
            effect.store_value = true;
            return WseDispatch::handled;
        }
        if (world->m6.ui.base_num_menus < 0) {
            world->m6.ui.base_num_menus = world->misc.num_menus;
        }
        const std::int32_t menu_id = world->misc.num_menus;
        world->misc.num_menus += 1;

        wse::game::WseM6DynamicMenu menu{};
        menu.id = "auto_menu_" + std::to_string(menu_id);
        menu.text = std::move(text);
        menu.mesh_name = std::move(mesh_name);
        menu.flags = (narrow.size() >= 4) ? narrow[3] : 0;
        menu.enter_script_no =
            (narrow.size() >= 5 && narrow[4] > 0) ? narrow[4] : -1;
        world->m6.ui.menus[menu_id] = std::move(menu);
        world->m6.ui.generated_menu_ids.push_back(menu_id);

        effect.value = menu_id;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::menu_add_item: {
        // 4801 (0x1401f2393)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string text;
        if (world == nullptr || !world->is_valid_menu(narrow[0]) ||
            !wse_resolve_parsed_utf8_operand(1, operands, text)) {
            return WseDispatch::ignored;
        }
        wse::game::WseM6DynamicMenuItem item{};
        item.text = std::move(text);
        item.condition_script_no =
            (narrow.size() >= 3 && narrow[2] > 0) ? narrow[2] : -1;
        item.consequence_script_no =
            (narrow.size() >= 4 && narrow[3] > 0) ? narrow[3] : -1;
        world->m6.ui.menus[narrow[0]].items.push_back(std::move(item));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::menu_clear_items: {
        // 4802 (0x1401f2683)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_menu(narrow[0])) {
            return WseDispatch::ignored;
        }
        if (const auto it = world->m6.ui.menus.find(narrow[0]);
            it != world->m6.ui.menus.end()) {
            it->second.items.clear();
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::menu_clear_generated: {
        // 4803 (0x1401f26de)
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        for (const std::int32_t id : world->m6.ui.generated_menu_ids) {
            world->m6.ui.menus.erase(id);
        }
        world->m6.ui.generated_menu_ids.clear();
        if (world->m6.ui.base_num_menus >= 0) {
            world->misc.num_menus = world->m6.ui.base_num_menus;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::overlay_get_val: {
        // 4900 (0x1401f2749)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_overlay(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& ov =
            world->overlays[static_cast<std::size_t>(narrow[1])];
        const bool has_val =
            (ov.type >= 2 && ov.type <= 14) &&
            !(ov.type == 4 && !ov.m5.is_checkbox);
        effect.value = has_val ? ov.val : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::presentation_activate: {
        // 4901 (0x1401f27ba)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0) {
            return WseDispatch::ignored;
        }
        if (world->m6.ui.active_window_id != -1 &&
            world->m6.ui.active_window_id != 10 &&
            world->m6.ui.active_window_id != 12) {
            return WseDispatch::condition_false;
        }
        const auto& prs = world->m6.ui.active_presentations;
        if (std::find(prs.begin(), prs.end(), narrow[0]) == prs.end()) {
            return WseDispatch::condition_false;
        }
        world->active_presentation_no = narrow[0];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::overlay_button_set_type: {
        // 4902 (0x1401f2888)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_overlay(narrow[0])) {
            return WseDispatch::ignored;
        }
        const auto type =
            world->overlays[static_cast<std::size_t>(narrow[0])].type;
        if (type != 1 && type != 9 && type != 10) {
            return WseDispatch::ignored;
        }
        world->m6.ui.overlay_ext[narrow[0]].button_type =
            ((narrow[2] != 0) ? 2 : 0) | ((narrow[1] == 0) ? 1 : 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::overlay_get_scroll_pos: {
        // 4903 (0x1401f2900)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_overlay(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& ov =
            world->overlays[static_cast<std::size_t>(narrow[1])];
        if (ov.type != 8) {
            effect.value = -1;
        } else {
            const auto it = world->m6.ui.overlay_ext.find(narrow[1]);
            const float scroll =
                (it == world->m6.ui.overlay_ext.end())
                    ? 0.0f
                    : ((narrow.size() >= 3 && narrow[2] != 0)
                           ? it->second.scroll_pos.x
                           : it->second.scroll_pos.y);
            effect.value = static_cast<std::int64_t>(scroll * fp_mul);
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::overlay_set_scroll_pos: {
        // 4904 (0x1401f2994)
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_overlay(narrow[0])) {
            return WseDispatch::ignored;
        }
        if (world->overlays[static_cast<std::size_t>(narrow[0])].type != 8) {
            return WseDispatch::ignored;
        }
        const float pos = static_cast<float>(wide[1]) / fp_mul;
        auto& ext = world->m6.ui.overlay_ext[narrow[0]];
        if (narrow.size() >= 3 && narrow[2] != 0) {
            ext.scroll_pos.x = pos;
        } else {
            ext.scroll_pos.y = pos;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::overlay_enable: {
        // 4905 (0x1401f2a38)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_overlay(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->m6.ui.overlay_ext[narrow[0]].enabled = (narrow[1] != 0);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::overlay_item_set_text: {
        // 4906 (0x1401f2a7b)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string text;
        if (world == nullptr || !world->is_valid_overlay(narrow[0]) ||
            !wse_resolve_parsed_utf8_operand(2, operands, text)) {
            return WseDispatch::ignored;
        }
        auto& items =
            world->overlays[static_cast<std::size_t>(narrow[0])].m5.items;
        if (narrow[1] >= 0 &&
            static_cast<std::size_t>(narrow[1]) < items.size()) {
            items[static_cast<std::size_t>(narrow[1])] =
                wse_utf8_to_utf16(text);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::start_conversation_presentation: {
        // 4907 (0x1401f2aee)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 ||
            (world->m6.ui.active_window_id != -1 &&
             world->m6.ui.active_window_id != 12)) {
            return WseDispatch::ignored;
        }
        world->m6.ui.conversation_presentation_started = true;
        world->active_presentation_no = narrow[0];
        auto& prs = world->m6.ui.active_presentations;
        if (std::find(prs.begin(), prs.end(), narrow[0]) == prs.end()) {
            prs.push_back(narrow[0]);
        }
        // Started by `game::UiSession` on the next mission frame (`updatePresentations`).
        world->m6.ui.pending_conversation_presentations.push_back(narrow[0]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::skin_set_blood_color: {
        // 5200 (0x1401f4900)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 ||
            narrow[0] >= world->num_skins) {
            return WseDispatch::ignored;
        }
        world->m6.skins.blood_colors[narrow[0]] =
            static_cast<std::uint32_t>(narrow[1]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::skeleton_model_set_bone_body_section: {
        // 5201 (0x1401f492c)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string skel;
        if (world == nullptr || narrow[1] < 0 || narrow[1] >= 64 ||
            narrow[2] < 0 || narrow[2] >= 4 ||
            !wse_resolve_parsed_utf8_operand(0, operands, skel)) {
            return WseDispatch::ignored;
        }
        if (!world->resource_skeleton_models.empty() &&
            !world->resource_skeleton_models.contains(skel)) {
            return WseDispatch::ignored;
        }
        world->m6.skins
            .bone_body_sections[skel][static_cast<std::size_t>(narrow[1])] =
            narrow[2];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::skeleton_model_clean_body_sections: {
        // 5202 (0x1401f4a9d)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string skel;
        if (world == nullptr ||
            !wse_resolve_parsed_utf8_operand(0, operands, skel)) {
            return WseDispatch::ignored;
        }
        if (!world->resource_skeleton_models.empty() &&
            !world->resource_skeleton_models.contains(skel)) {
            return WseDispatch::ignored;
        }
        world->m6.skins.bone_body_sections[skel].fill(0);
        return WseDispatch::handled_no_store;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
