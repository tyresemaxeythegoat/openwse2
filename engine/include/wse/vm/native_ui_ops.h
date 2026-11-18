// Native VM opcodes (Milestone 5): UI, presentations, cursors, dialogs, screens
// Implemented from WSE2 v1.1.5.6 mbStatement::execute decompilations (62 opcodes).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "wse/vm/block_vm.h"

namespace wse::vm {

// `rglWidget::animateTo*` (`0x14031d980` / `0x14031d9c0` / `0x14031da80` / `0x14031dac0`):
// the caller stored the target, duration, and the current value as the start; this restarts
// the clock and marks the channel running (`UiSession::step_presentation` interpolates it).
inline void native_ui_start_anim(game::WseOverlayM5Ext& m5, std::uint8_t bit, std::size_t slot) {
    m5.anim_active = static_cast<std::uint8_t>(m5.anim_active | bit);
    m5.anim_elapsed[slot] = 0.0f;
}

[[nodiscard]] inline std::int64_t native_ui_push_overlay(
    game::WsePresentationOverlay ov,
    std::uint32_t opcode,
    VmContext& ctx,
    std::uint64_t dst,
    std::span<std::int64_t> locals) {
    ov.m5.container_overlay_no = ctx.world.m5.current_container_overlay_no;
    ov.m5.create_opcode = static_cast<std::int32_t>(opcode);
    const auto id = static_cast<std::int64_t>(ctx.world.overlays.size());
    ctx.world.overlays.push_back(std::move(ov));
    store_lvalue(dst, id, ctx, locals);
    return id;
}

inline void native_ui_create_range_overlay(
    std::uint32_t opcode,
    std::int32_t ov_type,
    std::int64_t min_v,
    std::int64_t max_v,
    VmContext& ctx,
    std::uint64_t dst,
    std::span<std::int64_t> locals) {
    if (ctx.world.active_presentation_no < 0) {
        return;
    }
    game::WsePresentationOverlay ov{};
    ov.type = ov_type;
    ov.m5.min_value = static_cast<double>(min_v);
    ov.m5.max_value = static_cast<double>(max_v);
    ov.m5.boundaries_min = {static_cast<float>(min_v), static_cast<float>(max_v)};
    ov.val = static_cast<std::int32_t>(min_v);
    (void)native_ui_push_overlay(std::move(ov), opcode, ctx, dst, locals);
}

inline void native_ui_create_text_button_overlay(
    std::int32_t string_index,
    std::uint8_t tag,
    std::uint32_t widget_flags,
    std::int32_t button_opcode,
    VmContext& ctx,
    std::uint64_t dst,
    std::span<std::int64_t> locals,
    std::uint32_t highlight_rgb = 0xffffffffU) {
    if (ctx.world.active_presentation_no < 0 ||
        !wse_string_register_ok(string_index, tag, native_string_banks(ctx))) {
        return;
    }
    game::WsePresentationOverlay ov{};
    ov.type = 1;
    ov.text = native_resolve_and_parse(string_index, tag, ctx);
    ov.m5.widget_flags = widget_flags;
    ov.m5.is_button = true;
    ov.m5.button_opcode = button_opcode;
    ov.m5.highlight_color = highlight_rgb;
    (void)native_ui_push_overlay(std::move(ov), static_cast<std::uint32_t>(button_opcode), ctx,
                                 dst, locals);
}

inline void native_ui_create_simple_overlay(
    std::uint32_t opcode,
    std::int32_t ov_type,
    VmContext& ctx,
    std::uint64_t dst,
    std::span<std::int64_t> locals) {
    if (ctx.world.active_presentation_no < 0) {
        return;
    }
    game::WsePresentationOverlay ov{};
    ov.type = ov_type;
    (void)native_ui_push_overlay(std::move(ov), opcode, ctx, dst, locals);
}

inline void native_ui_create_tableau_overlay(
    std::uint32_t opcode,
    std::int32_t ov_type,
    std::int32_t mesh_no,
    std::int32_t tableau_no,
    std::int32_t val,
    VmContext& ctx,
    std::uint64_t dst,
    std::span<std::int64_t> locals) {
    auto& world = ctx.world;
    if (world.active_presentation_no < 0 ||
        (mesh_no != -1 && !world.is_valid_mesh(mesh_no)) ||
        !world.is_valid_tableau_material(tableau_no)) {
        return;
    }
    game::WsePresentationOverlay ov{};
    ov.type = ov_type;
    ov.m5.mesh_no = mesh_no;
    ov.m5.tableau_material_no = tableau_no;
    ov.val = val;
    (void)native_ui_push_overlay(std::move(ov), opcode, ctx, dst, locals);
}

[[nodiscard]] inline std::u16string native_ui_resolve_label_or_default(
    std::int32_t string_index,
    std::uint8_t tag,
    VmContext& ctx,
    std::string_view lang_key,
    std::u16string_view fallback) {
    if (string_index >= 1 &&
        wse_string_register_ok(string_index, tag, native_string_banks(ctx))) {
        return native_resolve_and_parse(string_index, tag, ctx);
    }
    if (const auto it = ctx.world.translations.find(std::string(lang_key));
        it != ctx.world.translations.end()) {
        return wse_utf8_to_utf16(it->second);
    }
    return std::u16string(fallback);
}

inline void native_ui_enqueue_troop_screen(game::WseWorldState& world,
                                           std::int32_t troop_no,
                                           std::int32_t window_id) {
    if (world.is_valid_troop(troop_no)) {
        world.m5.screen_entry_point_no = troop_no;
        world.enqueue_window(window_id);
    }
}

[[nodiscard]] inline NativeDispatch dispatch_native_ui_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    const std::uint8_t tag0 = wse_tag_at(tags, 0);
    const std::uint8_t tag1 = wse_tag_at(tags, 1);
    const std::uint8_t tag2 = wse_tag_at(tags, 2);
    auto& world = ctx.world;
    const auto banks = native_string_banks(ctx);
    const float fp = ctx.fixedPointMultiplier != 0
                         ? static_cast<float>(ctx.fixedPointMultiplier)
                         : 1.0f;

    switch (opcode) {
    case 75: // mouse_get_position (0x1401c6ac9)
        if (wse_register_ok(narrow[0])) {
            auto& o = ctx.positions[static_cast<std::size_t>(narrow[0])].o;
            o.x = world.m5.cursor_pos.x;
            o.y = world.m5.cursor_pos.y;
            o.z = 0.0f;
            o.w = 1.0f;
        }
        return NativeDispatch::handled;
    case 751: // mouse_get_world_projection (0x1401cbe49)
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& cam = world.misc.camera_transform;
            const game::CameraProjectionScales proj = game::camera_projection_scales(
                world.misc.camera_fov_deg, world.misc.viewport_width,
                world.misc.viewport_height);
            const float tan_half = proj.tan_half_fov;
            const float aspect = proj.aspect;
            const float ndc_x = (world.m5.cursor_pos.x - 0.5f) * 2.0f;
            const float ndc_y = (world.m5.cursor_pos.y - 0.375f) * (2.0f / 0.75f);
            // Warband rglTransform camera frame: s = right, f = up, -u = forward.
            wse::core::WseVec4 dir{
                -cam.u.x + cam.s.x * (ndc_x * aspect * tan_half) +
                    cam.f.x * (ndc_y * tan_half),
                -cam.u.y + cam.s.y * (ndc_x * aspect * tan_half) +
                    cam.f.y * (ndc_y * tan_half),
                -cam.u.z + cam.s.z * (ndc_x * aspect * tan_half) +
                    cam.f.z * (ndc_y * tan_half),
                0.0f};
            dir.normalize();
            ctx.positions[static_cast<std::size_t>(narrow[0])].o = cam.o;
            ctx.positions[static_cast<std::size_t>(narrow[1])].o =
                cam.o + dir * 1000.0f;
        }
        return NativeDispatch::handled;
    case 911: // create_mesh_overlay (0x1401cc3b1)
        if (world.active_presentation_no >= 0 && world.is_valid_mesh(narrow[1])) {
            game::WsePresentationOverlay ov{};
            ov.type = 0;
            ov.m5.mesh_no = narrow[1];
            (void)native_ui_push_overlay(std::move(ov), opcode, ctx, dst, locals);
        }
        return NativeDispatch::handled;
    case 912: { // create_button_overlay (0x1401cc402, callee 0x14020ad60)
        const std::uint32_t flags =
            statement.numOperands > 2
                ? ((static_cast<std::uint32_t>(narrow[2]) & 0x181cU) | 0x200U)
                : 0x200U;
        native_ui_create_text_button_overlay(narrow[1], tag1, flags, 912, ctx, dst,
                                             locals);
        return NativeDispatch::handled;
    }
    case 913: // create_image_button_overlay (0x1401cc49f)
        if (world.active_presentation_no >= 0 && world.is_valid_mesh(narrow[1]) &&
            world.is_valid_mesh(narrow[2])) {
            game::WsePresentationOverlay ov{};
            ov.type = 4;
            ov.m5.mesh_no = narrow[1];
            ov.m5.pressed_mesh_no = narrow[2];
            (void)native_ui_push_overlay(std::move(ov), opcode, ctx, dst, locals);
        }
        return NativeDispatch::handled;
    case 914: // create_slider_overlay (0x1401cc506)
        native_ui_create_range_overlay(opcode, 2, wide[1], wide[2], ctx, dst, locals);
        return NativeDispatch::handled;
    case 915: // create_progress_overlay (0x1401cc544)
        native_ui_create_range_overlay(opcode, 3, wide[1], wide[2], ctx, dst, locals);
        return NativeDispatch::handled;
    case 916: // create_combo_button_overlay (0x1401cc582)
        native_ui_create_simple_overlay(opcode, 5, ctx, dst, locals);
        return NativeDispatch::handled;
    case 917: // create_text_box_overlay (0x1401cc5ba)
        native_ui_create_simple_overlay(opcode, 6, ctx, dst, locals);
        return NativeDispatch::handled;
    case 918: // create_check_box_overlay (0x1401cc5f2)
        if (world.active_presentation_no >= 0 &&
            (statement.numOperands < 3 ||
             (world.is_valid_mesh(narrow[1]) && world.is_valid_mesh(narrow[2])))) {
            game::WsePresentationOverlay ov{};
            ov.type = 4;
            ov.m5.is_checkbox = true;
            if (statement.numOperands >= 3) {
                ov.m5.mesh_no = narrow[1];
                ov.m5.pressed_mesh_no = narrow[2];
            }
            (void)native_ui_push_overlay(std::move(ov), opcode, ctx, dst, locals);
        }
        return NativeDispatch::handled;
    case 919: // create_simple_text_box_overlay (0x1401cc659)
        native_ui_create_simple_overlay(opcode, 7, ctx, dst, locals);
        return NativeDispatch::handled;
    case 923: // overlay_set_hilight_color (0x1401cc766)
        if (world.is_valid_overlay(narrow[0])) {
            world.overlays[static_cast<std::size_t>(narrow[0])]
                .m5.highlight_color = static_cast<std::uint32_t>(narrow[1]);
        }
        return NativeDispatch::handled;
    case 924: // overlay_set_hilight_alpha (0x1401cc790)
        if (world.is_valid_overlay(narrow[0])) {
            world.overlays[static_cast<std::size_t>(narrow[0])]
                .m5.highlight_alpha = static_cast<float>(narrow[1]) / 255.0f;
        }
        return NativeDispatch::handled;
    case 928: // overlay_set_boundaries (0x1401cc8a0)
        if (world.is_valid_overlay(narrow[0])) {
            auto& ov = world.overlays[static_cast<std::size_t>(narrow[0])];
            const float lo = static_cast<float>(wide[1]) / fp;
            const float hi = static_cast<float>(wide[2]) / fp;
            ov.m5.boundaries_min = {lo, hi};
            ov.m5.boundaries_max = {hi, hi};
            ov.m5.min_value = static_cast<double>(lo);
            ov.m5.max_value = static_cast<double>(hi);
            if (ov.type == 2 || ov.type == 3) {
                // Safety deviation: saturating float -> int32 conversion.
                const auto to_i32 = [](float v) noexcept {
                    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
                        game::native_f2i_saturate(v),
                        std::numeric_limits<std::int32_t>::min(),
                        std::numeric_limits<std::int32_t>::max()));
                };
                const std::int32_t clo = to_i32(lo);
                const std::int32_t chi = to_i32(hi);
                if (ov.val < clo) {
                    ov.val = clo;
                } else if (ov.val > chi) {
                    ov.val = chi;
                }
            }
        }
        return NativeDispatch::handled;
    case 929: // overlay_set_area_size (0x1401cc8cf)
        if (world.is_valid_overlay(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& o = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            world.overlays[static_cast<std::size_t>(narrow[0])].m5.area_size = {
                o.x, o.y};
        }
        return NativeDispatch::handled;
    case 930: // overlay_set_mesh_rotation (0x1401cc92d, callee 0x14020e580)
        if (world.is_valid_overlay(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& src = ctx.positions[static_cast<std::size_t>(narrow[1])];
            auto& rot = world.overlays[static_cast<std::size_t>(narrow[0])]
                            .m5.mesh_transform;
            rot.s = src.s;
            rot.f = src.f;
            rot.u = src.u;
            rot.o = {0.0f, 0.0f, 0.0f, 1.0f};
        }
        return NativeDispatch::handled;
    case 931: // overlay_add_item (0x1401cc986)
        if (world.is_valid_overlay(narrow[0]) &&
            wse_string_register_ok(narrow[1], tag1, banks)) {
            world.overlays[static_cast<std::size_t>(narrow[0])].m5.items.push_back(
                native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    case 932: // overlay_animate_to_color (0x1401cca07)
        if (world.is_valid_overlay(narrow[0])) {
            auto& ov = world.overlays[static_cast<std::size_t>(narrow[0])];
            auto& m5 = ov.m5;
            m5.anim_color = static_cast<std::uint32_t>(narrow[2]);
            m5.anim_color_duration = static_cast<float>(wide[1]) / 1000.0f;
            m5.anim_color_start = ov.color;
            native_ui_start_anim(m5, game::kOverlayAnimColor, 0);
        }
        return NativeDispatch::handled;
    case 933: // overlay_animate_to_alpha (0x1401cca46)
        if (world.is_valid_overlay(narrow[0])) {
            auto& ov = world.overlays[static_cast<std::size_t>(narrow[0])];
            auto& m5 = ov.m5;
            m5.anim_alpha = static_cast<float>(narrow[2]) / 255.0f;
            m5.anim_alpha_duration = static_cast<float>(wide[1]) / 1000.0f;
            m5.anim_alpha_start = ov.alpha;
            native_ui_start_anim(m5, game::kOverlayAnimAlpha, 1);
        }
        return NativeDispatch::handled;
    case 934: // overlay_animate_to_highlight_color (0x1401cca85)
        if (world.is_valid_overlay(narrow[0])) {
            auto& m5 = world.overlays[static_cast<std::size_t>(narrow[0])].m5;
            m5.anim_highlight_color = static_cast<std::uint32_t>(narrow[2]);
            m5.anim_highlight_color_duration = static_cast<float>(wide[1]) / 1000.0f;
            m5.anim_highlight_color_start = m5.highlight_color;
            native_ui_start_anim(m5, game::kOverlayAnimHighlightColor, 2);
        }
        return NativeDispatch::handled;
    case 935: // overlay_animate_to_highlight_alpha (0x1401ccac4)
        if (world.is_valid_overlay(narrow[0])) {
            auto& m5 = world.overlays[static_cast<std::size_t>(narrow[0])].m5;
            m5.anim_highlight_alpha = static_cast<float>(narrow[2]) / 255.0f;
            m5.anim_highlight_alpha_duration = static_cast<float>(wide[1]) / 1000.0f;
            m5.anim_highlight_alpha_start = m5.highlight_alpha;
            native_ui_start_anim(m5, game::kOverlayAnimHighlightAlpha, 3);
        }
        return NativeDispatch::handled;
    case 936: // overlay_animate_to_size (0x1401ccb03)
        if (world.is_valid_overlay(narrow[0]) && wse_register_ok(narrow[2])) {
            auto& ov = world.overlays[static_cast<std::size_t>(narrow[0])];
            auto& m5 = ov.m5;
            const auto& o = ctx.positions[static_cast<std::size_t>(narrow[2])].o;
            m5.anim_size = {o.x, o.y};
            m5.anim_size_duration = static_cast<float>(wide[1]) / 1000.0f;
            m5.anim_size_start = ov.size;
            native_ui_start_anim(m5, game::kOverlayAnimSize, 4);
        }
        return NativeDispatch::handled;
    case 937: // overlay_animate_to_position (0x1401ccb76)
        if (world.is_valid_overlay(narrow[0]) && wse_register_ok(narrow[2])) {
            auto& ov = world.overlays[static_cast<std::size_t>(narrow[0])];
            auto& m5 = ov.m5;
            const auto& o = ctx.positions[static_cast<std::size_t>(narrow[2])].o;
            m5.anim_position = {o.x, o.y};
            m5.anim_position_duration = static_cast<float>(wide[1]) / 1000.0f;
            m5.anim_position_start = ov.position;
            native_ui_start_anim(m5, game::kOverlayAnimPosition, 5);
        }
        return NativeDispatch::handled;
    case 938: // create_image_button_overlay_with_tableau_material (0x1401ccbe9)
        native_ui_create_tableau_overlay(opcode, 4, narrow[1], narrow[2], narrow[3], ctx,
                                         dst, locals);
        return NativeDispatch::handled;
    case 939: // create_mesh_overlay_with_tableau_material (0x1401ccc5f)
        native_ui_create_tableau_overlay(opcode, 0, narrow[1], narrow[2], narrow[3], ctx,
                                         dst, locals);
        return NativeDispatch::handled;
    // Both build an `rglGameButtonWidget` whose text flags are fixed to 0x10 (centre) by
    // `createGameButtonOverlay` (0x14020c560) / `createIngameButtonOverlay` (0x14020c950).
    // Their highlight text colours are `0xff007700` / `0xff00ff00` (`setTextHighlightColor`).
    case 940: // create_game_button_overlay (0x1401cccd5)
        native_ui_create_text_button_overlay(narrow[1], tag1, 0x10U, 940, ctx, dst, locals,
                                             0x007700U);
        return NativeDispatch::handled;
    case 941: // create_in_game_button_overlay (0x1401ccd6d)
        native_ui_create_text_button_overlay(narrow[1], tag1, 0x10U, 941, ctx, dst, locals,
                                             0x00FF00U);
        return NativeDispatch::handled;
    case 942: // create_number_box_overlay (0x1401cce05)
        native_ui_create_range_overlay(opcode, 2, wide[1], wide[2], ctx, dst, locals);
        return NativeDispatch::handled;
    case 943: // create_listbox_overlay (0x1401cce43)
        native_ui_create_simple_overlay(opcode, 8, ctx, dst, locals);
        return NativeDispatch::handled;
    case 944: // create_mesh_overlay_with_item_id (0x1401cce7b)
        if (world.active_presentation_no >= 0 && world.is_valid_item(narrow[1])) {
            game::WsePresentationOverlay ov{};
            ov.type = 0;
            ov.m5.item_no = narrow[1];
            (void)native_ui_push_overlay(std::move(ov), opcode, ctx, dst, locals);
        }
        return NativeDispatch::handled;
    case 945: // set_container_overlay (0x1401ccecc)
        if (narrow[0] == -1 || world.is_valid_overlay(narrow[0])) {
            world.m5.current_container_overlay_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 946: // overlay_get_position (0x1401ccef7, callee 0x14020e970)
        if (wse_register_ok(narrow[0]) && world.is_valid_overlay(narrow[1])) {
            const auto& pos =
                world.overlays[static_cast<std::size_t>(narrow[1])].position;
            auto& o = ctx.positions[static_cast<std::size_t>(narrow[0])].o;
            o.x = pos.x;
            o.y = pos.y;
            o.z = 0.0f;
        }
        return NativeDispatch::handled;
    case 948: // create_combo_label_overlay (0x1401ccf64)
        native_ui_create_simple_overlay(opcode, 5, ctx, dst, locals);
        return NativeDispatch::handled;
    case 949: // overlay_obtain_focus (0x1401ccf9c)
        if (world.is_valid_overlay(narrow[0])) {
            auto& ov = world.overlays[static_cast<std::size_t>(narrow[0])];
            if (ov.type == 6 || ov.type == 7) {
                for (auto& existing : world.overlays) {
                    if (existing.m5.focused && &existing != &ov) {
                        existing.m5.commit_pending = true;
                    }
                    existing.m5.focused = false;
                }
                ov.m5.focused = true;
                world.m5.focused_overlay_no = narrow[0];
            }
        }
        return NativeDispatch::handled;
    case 950: // overlay_set_tooltip (0x1401ccfc1, callee 0x14020ea20)
        if (world.is_valid_overlay(narrow[0]) &&
            wse_string_register_ok(narrow[1], tag1, banks)) {
            world.overlays[static_cast<std::size_t>(narrow[0])].m5.tooltip =
                native_resolve_and_parse(narrow[1], tag1, ctx);
        }
        return NativeDispatch::handled;
    case 951: // overlay_set_container_overlay (0x1401cd042)
        if (world.is_valid_overlay(narrow[0]) &&
            (narrow[1] == -1 || world.is_valid_overlay(narrow[1]))) {
            world.overlays[static_cast<std::size_t>(narrow[0])]
                .m5.container_overlay_no = narrow[1];
        }
        return NativeDispatch::handled;
    case 952: // overlay_set_additional_render_height (0x1401cd087)
        if (world.is_valid_overlay(narrow[0]) && narrow[1] >= -99 &&
            narrow[1] < 999) {
            world.overlays[static_cast<std::size_t>(narrow[0])]
                .m5.additional_render_height = static_cast<float>(wide[1]);
        }
        return NativeDispatch::handled;
    case 956: // overlay_set_material (0x1401cd0dd)
        if (world.is_valid_overlay(narrow[0]) &&
            wse_string_register_ok(narrow[1], tag1, banks)) {
            world.overlays[static_cast<std::size_t>(narrow[0])].m5.material_name =
                native_m5_resolve_underscore_utf8(narrow[1], tag1, ctx);
        }
        return NativeDispatch::handled;
    case 1120: // tutorial_box (0x1401ce566)
        if (wse_string_register_ok(narrow[0], tag0, banks) &&
            !world.m5.yes_no_window_open) {
            world.m5.dialog_title =
                (statement.numOperands > 1 && narrow[1] >= 1 &&
                 wse_string_register_ok(narrow[1], tag1, banks))
                    ? native_resolve_and_parse(narrow[1], tag1, ctx)
                    : std::u16string{};
            world.m5.dialog_text = native_resolve_and_parse(narrow[0], tag0, ctx);
            world.m5.yes_no_is_question = false;
            world.m5.yes_no_window_open = true;
        }
        return NativeDispatch::handled;
    case 1121: // question_box (0x1401ce6ab)
        if (wse_string_register_ok(narrow[0], tag0, banks) &&
            !world.m5.yes_no_window_open) {
            world.m5.dialog_yes_text = native_ui_resolve_label_or_default(
                statement.numOperands > 1 ? narrow[1] : 0, tag1, ctx, "ui_yes",
                u"Yes");
            world.m5.dialog_no_text = native_ui_resolve_label_or_default(
                statement.numOperands > 2 ? narrow[2] : 0, tag2, ctx, "ui_no",
                u"No");
            world.m5.dialog_text = native_resolve_and_parse(narrow[0], tag0, ctx);
            world.m5.yes_no_is_question = true;
            world.m5.yes_no_window_open = true;
        }
        return NativeDispatch::handled;
    case 1122: // tutorial_message (0x1401ce903)
        if (narrow[0] == -1) {
            world.m5.message_box_text.clear();
            world.m5.message_box_color =
                statement.numOperands > 1 ? static_cast<std::uint32_t>(narrow[1]) : 0U;
            world.m5.message_box_flags = 0U;
        } else if (wse_string_register_ok(narrow[0], tag0, banks)) {
            world.m5.message_box_text =
                native_resolve_and_parse(narrow[0], tag0, ctx);
            world.m5.message_box_color =
                (statement.numOperands > 1 && narrow[1] != 0)
                    ? static_cast<std::uint32_t>(narrow[1])
                    : 0xffffffffU;
            world.m5.message_box_flags =
                statement.numOperands > 2 ? static_cast<std::uint32_t>(narrow[2]) : 0U;
        }
        return NativeDispatch::handled;
    case 1123: // tutorial_message_set_position (0x1401ce9cd)
        world.m5.message_box_position = {
            static_cast<float>(wide[0]) / 1000.0f,
            static_cast<float>(wide[1]) / 1000.0f};
        return NativeDispatch::handled;
    case 1124: // tutorial_message_set_size (0x1401cea02)
        world.m5.message_box_size = {
            static_cast<float>(wide[0]) / 1000.0f,
            static_cast<float>(wide[1]) / 1000.0f};
        return NativeDispatch::handled;
    case 1125: // tutorial_message_set_center_justify (0x1401cea37)
        world.m5.message_box_center_justify = (wide[0] != 0);
        return NativeDispatch::handled;
    case 1126: // tutorial_message_set_background (0x1401cea46)
        world.m5.message_box_bg_mesh =
            (wide[0] == 0) ? std::string{} : std::string("white_plane");
        return NativeDispatch::handled;
    case 2040: // change_screen_return (0x1401dc07e, callee 0x1400dbe90)
        world.enqueue_window(1);
        return NativeDispatch::handled;
    case 2041: // change_screen_loot (0x1401dc09c)
        native_ui_enqueue_troop_screen(world, narrow[0], 2);
        return NativeDispatch::handled;
    case 2042: // change_screen_trade (0x1401dc0cf)
        native_ui_enqueue_troop_screen(world, narrow[0], 4);
        return NativeDispatch::handled;
    case 2043: { // change_screen_exchange_members (0x1401dc102)
        const std::int32_t party_no =
            statement.numOperands > 1 ? narrow[1] : world.main_party_no;
        if (world.is_valid_party(party_no)) {
            world.m5.screen_note_object =
                (party_no > 0) ? party_no
                               : (world.encountered_party_is_attacker ? 1 : 0);
            world.enqueue_window((wide[0] != 0) ? 6 : 5);
        }
        return NativeDispatch::handled;
    }
    case 2044: // change_screen_trade_prisoners (0x1401dc152)
        world.enqueue_window(9);
        return NativeDispatch::handled;
    case 2045: // change_screen_buy_mercenaries (0x1401dc16b)
        world.enqueue_window(10);
        return NativeDispatch::handled;
    case 2046: // change_screen_view_character (0x1401dc184)
        world.m5.screen_entry_point_no = narrow[0];
        world.enqueue_window(0xb);
        return NativeDispatch::handled;
    case 2047: // change_screen_training (0x1401dc1a4)
        world.enqueue_window(0xc);
        return NativeDispatch::handled;
    case 2048: // change_screen_mission (0x1401dc1bd)
        world.enqueue_window(0xd);
        return NativeDispatch::handled;
    case 2049: // change_screen_map_conversation (0x1401dc1d6)
        native_ui_enqueue_troop_screen(world, narrow[0], 0xe);
        return NativeDispatch::handled;
    case 2051: // change_screen_equip_other (0x1401dc23c)
        native_ui_enqueue_troop_screen(world, narrow[0], 3);
        return NativeDispatch::handled;
    case 2052: // change_screen_map (0x1401dc26f)
        world.enqueue_window(0x11);
        return NativeDispatch::handled;
    case 2053: { // change_screen_notes (0x1401dc288)
        bool ok = false;
        switch (narrow[0]) {
        case 1:
            ok = world.is_valid_troop(narrow[1]);
            break;
        case 2:
            ok = world.is_valid_faction(narrow[1]);
            break;
        case 3:
            ok = world.is_valid_party(narrow[1]);
            break;
        case 4:
            ok = world.is_valid_quest(narrow[1]);
            break;
        case 5:
            ok = world.is_valid_info_page(narrow[1]);
            break;
        default:
            break;
        }
        if (ok) {
            world.enqueue_window(0x10);
            world.m5.screen_note_kind = narrow[0];
            world.m5.screen_note_object = narrow[1];
        }
        return NativeDispatch::handled;
    }
    case 2055: // change_screen_quit (0x1401dc334)
        world.m5.enqueued_windows.clear();
        world.m5.quit_requested = true;
        world.m5.loading_mode = 5;
        world.m5.loading_savegame_no = 0;
        world.m5.active_window = game::kScreenWindowLoading;
        world.m6.ui.active_window_id = game::kScreenWindowLoading;
        return NativeDispatch::handled;
    case 2056: { // change_screen_give_members (0x1401dc3bc)
        const std::int32_t party_no =
            statement.numOperands > 0 ? narrow[0] : world.main_party_no;
        if (world.is_valid_party(party_no)) {
            world.m5.screen_note_object =
                (party_no > 0) ? party_no
                               : (world.encountered_party_is_attacker ? 1 : 0);
            world.enqueue_window(7);
        }
        return NativeDispatch::handled;
    }
    case 2057: // change_screen_controls (0x1401dc3ff)
        world.enqueue_window(0x13);
        return NativeDispatch::handled;
    case 2058: // change_screen_options (0x1401dc418)
        world.enqueue_window(0x14);
        return NativeDispatch::handled;
    default:
        break;
    }

    return NativeDispatch::unimplemented;
}

}  // namespace wse::vm
