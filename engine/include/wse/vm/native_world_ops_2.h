// Extended native Warband opcode handlers (mission_combat_and_teams,
// audio_particles_and_tableau, game_state_and_stores). Grounded in official
// WSE2 v1.1.5.6 PDB/binary decompilation:
//   mission_combat_and_teams (8):   1784, 1786, 1787, 1828, 1837, 1900, 1971, 1976
//   audio_particles_and_tableau (26): 601..604, 609, 1960, 1961, 1965, 1968, 1975,
//                                    1980, 1984, 1985, 1987..1994, 1996..2000
//   game_state_and_stores (35):     50, 55, 58, 61, 62, 90..93, 615, 1030, 1031,
//                                   1490, 2070..2073, 2155, 2159, 2167, 2199, 2211,
//                                   2220, 2240, 2271, 2300..2302, 2360, 2380..2385

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_world_3_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

// Mirrors Warband trigger-entity validation across callers (e.g. opcodes 1965/1968/1984/1985): checks whether the active trigger
// caller carries a scene-prop, mission-object, or agent entity context.
[[nodiscard]] inline bool native_world_has_trigger_entity(
    const game::WseWorldState& world,
    std::int32_t caller) noexcept {
    switch (caller) {
    case -104:
    case -101:
    case -100:
    case -76:
    case -48:
    case -47:
    case -46:
    case -45:
    case -44:
    case -43:
    case -42:
    case -40:
        return world.lighting.has_scene && !world.mission_objects.empty();
    case -51:
        return world.is_valid_agent(world.fx.trigger_agent_no);
    case -50:
        return world.is_valid_agent(world.fx.trigger_agent_no) ||
               (world.lighting.has_scene && !world.mission_objects.empty());
    default:
        return false;
    }
}

[[nodiscard]] inline std::int32_t native_world_highest_remaining_team_no(
    const game::WseWorldState& world) noexcept {
    std::array<bool, game::kNumTeams> has_alive{};
    for (const auto& ag : world.agents) {
        if (ag.active && ag.is_human() && ag.is_alive() &&
            world.is_valid_team(ag.team_no)) {
            has_alive[static_cast<std::size_t>(ag.team_no)] = true;
        }
    }
    std::int32_t highest = -1;
    for (std::int32_t t = 0; t < game::kNumTeams; ++t) {
        if (has_alive[static_cast<std::size_t>(t)]) {
            highest = t;
        }
    }
    return highest;
}

[[nodiscard]] inline std::int64_t native_world_count_team(
    const VmContext& ctx,
    std::int32_t team_no) noexcept {
    return static_cast<std::int64_t>(game::mission_count_normalized_team_members(
        ctx.world, ctx.options.battle_size, team_no));
}

[[nodiscard]] inline NativeDispatch native_world_read_team_sub_order(
    const std::array<std::int32_t, 9>& arr,
    std::int32_t division,
    std::int32_t valid_upper_exclusive,
    std::uint64_t dst,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const std::int32_t div = std::clamp(division, 0, 8);
    const std::int32_t val = arr[static_cast<std::size_t>(div)];
    const std::int32_t out =
        (valid_upper_exclusive <= 0 || (val >= 0 && val < valid_upper_exclusive))
            ? val
            : -1;
    store_lvalue(dst, static_cast<std::int64_t>(out), ctx, locals);
    return NativeDispatch::handled;
}

inline void native_world_push_tableau_mesh(
    game::WseWorldState& world,
    std::int32_t mesh_no,
    std::int32_t pos_reg_no,
    const wse::core::WseVec4& scale,
    float alpha,
    std::uint32_t vertex_color,
    const VmContext& ctx) {
    game::WseTableauRenderItem item{};
    item.kind = game::WseTableauRenderItem::Kind::mesh;
    item.id = mesh_no;
    item.position = ctx.positions[static_cast<std::size_t>(pos_reg_no)];
    item.scale = scale;
    item.alpha = alpha;
    item.vertex_color = vertex_color;
    game::push_bounded(world.tableau.items, std::move(item), game::kMaxTableauItems);
}

[[nodiscard]] inline NativeDispatch dispatch_native_world_2_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    const std::uint8_t tag1 = tags.size() > 1 ? tags[1] : 0U;
    auto& world = ctx.world;
    const float fp =
        ctx.fixedPointMultiplier != 0
            ? static_cast<float>(ctx.fixedPointMultiplier)
            : 1.0f;

    switch (opcode) {
    // -----------------------------------------------------------------
    // game_state_and_stores (35 opcodes)
    // -----------------------------------------------------------------
    case 50: // store_repeat_object (0x1401c6773)
        store_lvalue(dst, world.misc.repeat_object, ctx, locals);
        return NativeDispatch::handled;
    case 55: // get_operation_set_version (0x1401c67a1)
        store_lvalue(dst, 9999, ctx, locals);
        return NativeDispatch::handled;
    case 58: // set_physics_delta_time (0x1401c67c8)
        world.misc.physics_delta_time = static_cast<float>(wide[0]) / fp;
        return NativeDispatch::handled;
    case 61: // is_camera_in_first_person (0x1401c6852)
        return world.misc.camera_first_person ? NativeDispatch::handled
                                              : NativeDispatch::condition_false;
    case 62: { // set_camera_in_first_person (0x1401c6864)
        const bool first_person = (narrow[0] != 0);
        if (world.misc.camera_first_person != first_person) {
            world.misc.camera_first_person = first_person;
            if (world.is_valid_agent(world.player_agent_no)) {
                world.agents[static_cast<std::size_t>(world.player_agent_no)]
                    .camera_mode = first_person;
            }
        }
        return NativeDispatch::handled;
    }
    case 90: // get_global_cloud_amount (0x1401c6b73)
        store_lvalue(dst,
                     game::native_f2i_saturate(world.lighting.cloud_amount * 100.0f),
                     ctx, locals);
        return NativeDispatch::handled;
    case 91: // set_global_cloud_amount (0x1401c6bb1)
        world.lighting.cloud_amount =
            static_cast<float>(std::clamp(narrow[0], 0, 99)) / 100.0f;
        return NativeDispatch::handled;
    case 92: // get_global_haze_amount (0x1401c6be2)
        store_lvalue(dst,
                     game::native_f2i_saturate(world.lighting.haze_amount * 100.0f),
                     ctx, locals);
        return NativeDispatch::handled;
    case 93: // set_global_haze_amount (0x1401c6c20)
        world.lighting.haze_amount =
            static_cast<float>(std::clamp(narrow[0], 0, 99)) / 100.0f;
        return NativeDispatch::handled;

    // -----------------------------------------------------------------
    // audio_particles_and_tableau (audio part: 601..609) + 615 store
    // -----------------------------------------------------------------
    case 601: // play_track (0x1401ca9c3)
        if (world.is_valid_music_track(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < 3) {
            world.music.current_track_no = narrow[0];
            world.music.track_options = narrow[1];
            if (world.audio_runtime != nullptr) {
                world.audio_runtime->play_track(narrow[0], narrow[1]);
            }
        }
        return NativeDispatch::handled;
    case 602: // play_cue_track (0x1401caa03)
        if (world.is_valid_music_track(narrow[0])) {
            world.music.cue_track_no = narrow[0];
            if (world.audio_runtime != nullptr) {
                world.audio_runtime->play_cue_track(narrow[0]);
            }
        }
        return NativeDispatch::handled;
    case 603: // music_set_situation (0x1401caa22)
        world.music.situation_flags = static_cast<std::uint32_t>(narrow[0]);
        if (world.audio_runtime != nullptr) {
            world.audio_runtime->set_music_situation(world.music.situation_flags);
        }
        return NativeDispatch::handled;
    case 604: // music_set_culture (0x1401caa2e)
        world.music.culture_flags = static_cast<std::uint32_t>(narrow[0]);
        if (world.audio_runtime != nullptr) {
            world.audio_runtime->set_music_culture(world.music.culture_flags);
        }
        return NativeDispatch::handled;
    case 609: // stop_all_sounds (0x1401caa3a)
        world.music.all_sounds_stopped = (narrow[0] != 0);
        if (world.audio_runtime != nullptr) {
            world.audio_runtime->stop_all_sounds(narrow[0] != 0);
        }
        return NativeDispatch::handled;
    case 615: // store_last_sound_channel (0x1401caa51)
        if (world.audio_runtime != nullptr) {
            world.music.last_sound_channel =
                world.audio_runtime->last_sound_channel();
        }
        store_lvalue(dst, static_cast<std::int64_t>(world.music.last_sound_channel),
                     ctx, locals);
        return NativeDispatch::handled;

    // -----------------------------------------------------------------
    // game_state_and_stores (world/campaign/mission stores)
    // -----------------------------------------------------------------
    case 1030: // rest_for_hours (0x1401cd66f)
    case 1031: // rest_for_hours_interactive (0x1401cd6e6)
        world.misc.rest_hours = narrow[0];
        world.misc.rest_time_speed = narrow[1];
        world.misc.rest_interactive = (opcode == 1031U);
        if (narrow[0] > 0 && world.is_valid_party(world.main_party_no)) {
            world.parties[static_cast<std::size_t>(world.main_party_no)].behavior =
                (wide[2] == 0) ? 8 : 0;
        }
        return NativeDispatch::handled;
    case 1490: { // set_merchandise_modifier_quality (0x1401d0133)
        // Kept as a float fraction: truncating to an integer turned 150 into 1
        // and anything below 100 into 0.
        world.misc.merchandise_modifier_quality =
            std::clamp(static_cast<float>(wide[0]) / 100.0f, 0.0f, 1000.0f);
        return NativeDispatch::handled;
    }

    // -----------------------------------------------------------------
    // mission_combat_and_teams (8 opcodes)
    // -----------------------------------------------------------------
    case 1784: // team_get_hold_fire_order (0x1401d5f9c)
        if (!world.is_valid_team(narrow[1])) {
            return NativeDispatch::handled;
        }
        return native_world_read_team_sub_order(
            world.teams[static_cast<std::size_t>(narrow[1])].sub_orders.hold_fire,
            narrow[2], 2, dst, ctx, locals);
    case 1786: // team_get_riding_order (0x1401d602c)
        if (!world.is_valid_team(narrow[1])) {
            return NativeDispatch::handled;
        }
        return native_world_read_team_sub_order(
            world.teams[static_cast<std::size_t>(narrow[1])].sub_orders.riding,
            narrow[2], 3, dst, ctx, locals);
    case 1787: // team_get_weapon_usage_order (0x1401d6074)
        if (!world.is_valid_team(narrow[1])) {
            return NativeDispatch::handled;
        }
        return native_world_read_team_sub_order(
            world.teams[static_cast<std::size_t>(narrow[1])]
                .sub_orders.weapon_usage,
            narrow[2], 4, dst, ctx, locals);
    case 1828: // team_get_gap_distance (0x1401d74fe)
        if (!world.is_valid_team(narrow[1]) || narrow[2] < 0 ||
            narrow[2] >= game::kNumDivisions) {
            return NativeDispatch::handled;
        }
        return native_world_read_team_sub_order(
            world.teams[static_cast<std::size_t>(narrow[1])].sub_orders.gap_distance,
            narrow[2], 0, dst, ctx, locals);
    case 1837: // class_set_name (0x1401d78c4)
        if (narrow[0] >= 0 && narrow[0] < game::kNumDivisions &&
            wse_string_register_ok(narrow[1], tag1, native_string_banks(ctx))) {
            world.class_names[static_cast<std::size_t>(narrow[0])] =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    case 1900: { // cast_ray (0x1401d9260)
        if (!wse_register_ok(narrow[1]) || !wse_register_ok(narrow[2])) {
            return NativeDispatch::handled;
        }
        const float max_dist =
            (wide[3] == 0) ? 10000.0f : (static_cast<float>(wide[3]) / fp);
        wse::core::WseVec4 dir =
            ctx.positions[static_cast<std::size_t>(narrow[2])].f;
        dir.normalize();
        const std::uint32_t mask =
            static_cast<std::uint32_t>(narrow[4]) | 0x1200100U;
        const NativeRayHitResult hit = native_world_ray_aabb_hit(
            world, ctx.positions[static_cast<std::size_t>(narrow[2])].o, dir,
            max_dist, mask);
        std::int32_t prop_no = -1;
        if (!hit.hit) {
            store_lvalue(dst, -1, ctx, locals);
            return NativeDispatch::condition_false;
        }
        if (hit.is_prop && world.is_valid_mission_object(hit.object_no) &&
            !world.mission_objects[static_cast<std::size_t>(hit.object_no)].is_item) {
            prop_no = hit.object_no;
        }
        auto& out = ctx.positions[static_cast<std::size_t>(narrow[1])];
        out.o = hit.point;
        out.f = {-hit.normal.x, -hit.normal.y, -hit.normal.z, -hit.normal.w};
        out.u = {0.0f, 0.0f, 1.0f, 0.0f};
        out.orthonormalize();
        store_lvalue(dst, static_cast<std::int64_t>(prop_no), ctx, locals);
        return NativeDispatch::handled;
    }

    // -----------------------------------------------------------------
    // point lights, particle systems & spawned items (1960..1976)
    // -----------------------------------------------------------------
    case 1960: // add_point_light (0x1401d9951)
    case 1961: // add_point_light_to_entity (0x1401d9a66)
        if (narrow[0] >= 0 && narrow[1] >= 0 &&
            (opcode == 1960U ||
             native_world_has_trigger_entity(world, ctx.currentCaller))) {
            game::WsePointLightEntry entry{};
            entry.caller = ctx.currentCaller;
            entry.r = world.fx.trigger_current_color.x;
            entry.g = world.fx.trigger_current_color.y;
            entry.b = world.fx.trigger_current_color.z;
            entry.flicker_magnitude = static_cast<float>(wide[0]) / 100.0f;
            entry.flicker_interval = static_cast<float>(wide[1]) / 100.0f;
            entry.specular_coefficient = static_cast<float>(wide[2]);
            entry.position = world.fx.trigger_transform;
            entry.to_entity = (opcode == 1961U);
            game::push_bounded(world.fx.point_lights, entry, game::kMaxPointLights);
        }
        return NativeDispatch::handled;
    case 1965: // particle_system_add_new (0x1401d9c41)
        if (world.is_valid_particle_system(narrow[0])) {
            game::WseParticleSystemEntry ev{};
            ev.kind_no = narrow[0];
            ev.attach_type = (ctx.currentCaller == -70) ? 0 : 2;
            ev.caller = ctx.currentCaller;
            ev.transform = world.fx.trigger_transform;
            ev.synced = true;
            game::push_bounded(world.fx.particle_events, ev,
                               game::kMaxParticleEvents);
        }
        return NativeDispatch::handled;
    case 1968: // particle_system_emit (0x1401d9de8)
        if (world.is_valid_particle_system(narrow[0]) &&
            native_world_has_trigger_entity(world, ctx.currentCaller)) {
            for (auto& ev : world.fx.particle_events) {
                if (ev.kind_no == narrow[0]) {
                    ev.emit_amount += narrow[1];
                }
            }
        }
        return NativeDispatch::handled;
    case 1971: // spawn_item (0x1401da062)
    case 1976: // spawn_item_without_refill (0x1401da1fc)
        if (world.is_valid_item(narrow[0]) && narrow[2] >= 0 &&
            world.lighting.has_scene) {
            const std::int32_t obj_no = game::mission_spawn_item(
                world, narrow[0], narrow[1], opcode == 1971U, narrow[2]);
            ctx.registers[0] = static_cast<std::int64_t>(obj_no);
        }
        return NativeDispatch::handled;
    case 1975: // particle_system_burst_no_sync (0x1401da189)
        if (world.is_valid_particle_system(narrow[0]) && wse_register_ok(narrow[1])) {
            const std::int32_t strength = (narrow[2] != 0) ? narrow[2] : 100;
            game::mission_burst_particle_system(
                world, narrow[0], ctx.positions[static_cast<std::size_t>(narrow[1])],
                strength, false);
        }
        return NativeDispatch::handled;

    // -----------------------------------------------------------------
    // cur_tableau_* (1980, 1984, 1985, 1987..1994, 1996..2000)
    // -----------------------------------------------------------------
    case 1980: // cur_tableau_add_tableau_mesh (0x1401da41e)
        if (world.tableau.active && world.is_valid_tableau_material(narrow[0]) &&
            wse_register_ok(narrow[2])) {
            const float s = (wide[3] == 0) ? 1.0f : (static_cast<float>(wide[3]) / fp);
            game::WseTableauRenderItem item{};
            item.kind = game::WseTableauRenderItem::Kind::tableau_mesh;
            item.id = narrow[0];
            item.param = narrow[1];
            item.position = ctx.positions[static_cast<std::size_t>(narrow[2])];
            item.scale = {s, s, s, s};
            game::push_bounded(world.tableau.items, std::move(item),
                               game::kMaxTableauItems);
        }
        return NativeDispatch::handled;
    case 1984: // cur_tableau_render_as_alpha_mask (0x1401da639)
        if (world.tableau.active) {
            world.tableau.render_as_alpha_mask = true;
        }
        return NativeDispatch::handled;
    case 1985: // cur_tableau_set_background_color (0x1401da65e)
        if (world.tableau.active) {
            world.tableau.background_color = static_cast<std::uint32_t>(narrow[0]);
        }
        return NativeDispatch::handled;
    case 1987: // cur_tableau_set_ambient_light (0x1401da719)
        if (world.tableau.active) {
            world.tableau.ambient_r = static_cast<float>(wide[0]) / fp;
            world.tableau.ambient_g = static_cast<float>(wide[1]) / fp;
            world.tableau.ambient_b = static_cast<float>(wide[2]) / fp;
        }
        return NativeDispatch::handled;
    case 1988: // cur_tableau_set_camera_position (0x1401da78c)
        if (world.tableau.active && wse_register_ok(narrow[0])) {
            world.tableau.camera_position =
                ctx.positions[static_cast<std::size_t>(narrow[0])];
        }
        return NativeDispatch::handled;
    case 1989: // cur_tableau_set_camera_parameters (0x1401da7d4)
        if (world.tableau.active) {
            world.tableau.camera_orthogonal = (wide[0] != 0);
            world.tableau.camera_fov_or_width = static_cast<float>(wide[1]) / fp;
            world.tableau.camera_aspect = static_cast<float>(wide[2]) / fp;
            world.tableau.camera_near = static_cast<float>(wide[3]) / fp;
            world.tableau.camera_far = static_cast<float>(wide[4]) / fp;
        }
        return NativeDispatch::handled;
    case 1990: // cur_tableau_add_point_light (0x1401da860)
    case 1991: // cur_tableau_add_sun_light (0x1401da8f6)
        if (world.tableau.active && wse_register_ok(narrow[0])) {
            game::WseTableauLight light{};
            light.is_sun = (opcode == 1991U);
            light.position = ctx.positions[static_cast<std::size_t>(narrow[0])];
            light.r = static_cast<float>(wide[1]) / fp;
            light.g = static_cast<float>(wide[2]) / fp;
            light.b = static_cast<float>(wide[3]) / fp;
            game::push_bounded(world.tableau.lights, light,
                               game::kMaxTableauItems);
        }
        return NativeDispatch::handled;
    case 1992: // cur_tableau_add_mesh (0x1401da98c)
    case 1993: { // cur_tableau_add_mesh_with_vertex_color (0x1401daa60)
        if (world.tableau.active && world.is_valid_mesh(narrow[0]) &&
            wse_register_ok(narrow[1])) {
            const float s = (wide[2] == 0) ? 1.0f : (static_cast<float>(wide[2]) / fp);
            const float a = (wide[3] == 0) ? 1.0f : (static_cast<float>(wide[3]) / fp);
            const std::uint32_t vcol =
                (opcode == 1992U) ? 0xffffffffu
                                  : static_cast<std::uint32_t>(narrow[4]);
            native_world_push_tableau_mesh(world, narrow[0], narrow[1],
                                           {s, s, s, s}, a, vcol, ctx);
        }
        return NativeDispatch::handled;
    }
    case 1994: // cur_tableau_add_map_icon (0x1401dab35)
        if (world.tableau.active && world.is_valid_map_icon(narrow[0]) &&
            wse_register_ok(narrow[1])) {
            const float s = (wide[2] == 0) ? 1.0f : (static_cast<float>(wide[2]) / fp);
            game::WseTableauRenderItem item{};
            item.kind = game::WseTableauRenderItem::Kind::map_icon;
            item.id = narrow[0];
            item.position = ctx.positions[static_cast<std::size_t>(narrow[1])];
            item.scale = {s, s, s, s};
            game::push_bounded(world.tableau.items, std::move(item),
                               game::kMaxTableauItems);
        }
        return NativeDispatch::handled;
    case 1996: // cur_tableau_add_horse (0x1401dac4c)
        if (world.tableau.active && world.is_valid_item(narrow[0]) &&
            wse_register_ok(narrow[1]) && world.is_valid_action(narrow[2])) {
            game::WseTableauRenderItem item{};
            item.kind = game::WseTableauRenderItem::Kind::horse;
            item.id = -1;
            item.override_horse_item_no = narrow[0];
            item.position = ctx.positions[static_cast<std::size_t>(narrow[1])];
            item.param = narrow[2];
            game::push_bounded(world.tableau.items, std::move(item),
                               game::kMaxTableauItems);
        }
        return NativeDispatch::handled;
    case 1997: // cur_tableau_set_override_flags (0x1401daccd)
        if (world.tableau.active) {
            world.tableau.override_flags = static_cast<std::uint32_t>(narrow[0]);
        }
        return NativeDispatch::handled;
    case 1998: // cur_tableau_clear_override_items (0x1401dacf6)
        if (world.tableau.active) {
            world.tableau.override_items.clear();
        }
        return NativeDispatch::handled;
    case 1999: // cur_tableau_add_override_item (0x1401dad1b)
        if (world.tableau.active) {
            game::push_bounded(world.tableau.override_items, narrow[0],
                               game::kMaxTableauOverrideItems);
        }
        return NativeDispatch::handled;
    case 2000: { // cur_tableau_add_mesh_with_scale_and_vertex_color (0x1401dad44)
        if (world.tableau.active && world.is_valid_mesh(narrow[0]) &&
            wse_register_ok(narrow[1]) && wse_register_ok(narrow[2])) {
            const auto& so = ctx.positions[static_cast<std::size_t>(narrow[2])].o;
            const wse::core::WseVec4 scale{so.x / fp, so.y / fp, so.z / fp,
                                           so.w / fp};
            const float a = (wide[3] == 0) ? 1.0f : (static_cast<float>(wide[3]) / fp);
            native_world_push_tableau_mesh(world, narrow[0], narrow[1], scale, a,
                                           static_cast<std::uint32_t>(narrow[4]),
                                           ctx);
        }
        return NativeDispatch::handled;
    }

    // -----------------------------------------------------------------
    // trigger params & store_* queries (2070..2385)
    // -----------------------------------------------------------------
    case 2070: // store_trigger_param (0x1401dc63f)
        if (narrow[1] >= 1 && narrow[1] < 17) {
            store_lvalue(dst,
                         world.misc.trigger_params[static_cast<std::size_t>(narrow[1] - 1)],
                         ctx, locals);
        } else {
            ctx.report_error("Invalid Trigger Parameter ID: " +
                             std::to_string(narrow[1]));
        }
        return NativeDispatch::handled;
    case 2071: // store_trigger_param_1 (0x1401dc69a)
        store_lvalue(dst, world.misc.trigger_params[0], ctx, locals);
        return NativeDispatch::handled;
    case 2072: // store_trigger_param_2 (0x1401dc6b1)
        store_lvalue(dst, world.misc.trigger_params[1], ctx, locals);
        return NativeDispatch::handled;
    case 2073: // store_trigger_param_3 (0x1401dc6c8)
        store_lvalue(dst, world.misc.trigger_params[2], ctx, locals);
        return NativeDispatch::handled;
    case 2155: // store_num_free_prisoner_stacks (0x1401dd79e)
        store_lvalue(dst, 10, ctx, locals);
        return NativeDispatch::handled;
    case 2159: { // store_num_regular_prisoners (0x1401dd8b6)
        std::int64_t total = 0;
        if (world.is_valid_party(world.main_party_no)) {
            const auto& main_p =
                world.parties[static_cast<std::size_t>(world.main_party_no)];
            for (const auto& st : main_p.stacks) {
                if (st.is_prisoner() && world.is_valid_troop(st.troop_no) &&
                    !world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
                    total += static_cast<std::int64_t>(st.num_troops);
                }
            }
        }
        store_lvalue(dst, total, ctx, locals);
        return NativeDispatch::handled;
    }
    case 2167: // store_free_inventory_capacity (0x1401dda16)
        if (world.is_valid_troop(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             game::troop_free_inventory_capacity(world, narrow[1])),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2199: // store_conversation_agent (0x1401dde1b)
        store_lvalue(dst, static_cast<std::int64_t>(world.dialog.speaker_agent_no),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2211: // store_current_scene (0x1401ddf46)
        store_lvalue(dst, static_cast<std::int64_t>(world.current_site_no), ctx,
                     locals);
        return NativeDispatch::handled;
    case 2220: // store_zoom_amount (0x1401ddf9a)
        store_lvalue(dst, game::native_f2i_saturate(fp * world.zoom_amount), ctx,
                     locals);
        return NativeDispatch::handled;
    case 2240: { // store_partner_quest (0x1401de0a0)
        std::int32_t found = -1;
        for (std::int32_t qi = 0; qi < static_cast<std::int32_t>(world.quests.size());
             ++qi) {
            const auto& q = world.quests[static_cast<std::size_t>(qi)];
            if (q.is_active() && (q.template_flags & 2U) != 0U &&
                q.giver_troop_no == world.conversation_partner_troop_no) {
                found = qi;
            }
        }
        store_lvalue(dst, static_cast<std::int64_t>(found), ctx, locals);
        return NativeDispatch::handled;
    }
    case 2271: // store_time_of_day (0x1401de41b)
        store_lvalue(dst, static_cast<std::int64_t>(world.current_hours % 24),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2300: // store_num_parties_created (0x1401de6ef)
        if (world.is_valid_party_template(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.party_templates[static_cast<std::size_t>(narrow[1])]
                                 .num_created),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2301: // store_num_parties_destroyed (0x1401de73a)
        if (world.is_valid_party_template(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.party_templates[static_cast<std::size_t>(narrow[1])]
                                 .num_destroyed),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2302: // store_num_parties_destroyed_by_player (0x1401de785)
        if (world.is_valid_party_template(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.party_templates[static_cast<std::size_t>(narrow[1])]
                                 .num_destroyed_by_player),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 2360: // store_remaining_team_no (0x1401df3b1)
        store_lvalue(dst,
                     static_cast<std::int64_t>(
                         native_world_highest_remaining_team_no(world)),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2380: // store_enemy_count (0x1401df5a8)
        store_lvalue(dst, native_world_count_team(ctx, 1), ctx, locals);
        return NativeDispatch::handled;
    case 2381: // store_friend_count (0x1401df5e0)
        store_lvalue(dst, native_world_count_team(ctx, 0), ctx, locals);
        return NativeDispatch::handled;
    case 2382: // store_ally_count (0x1401df611)
        store_lvalue(dst, 0, ctx, locals);
        return NativeDispatch::handled;
    case 2383: { // store_defender_count (0x1401df631)
        const std::int32_t team =
            (world.main_party_side == 1 ||
             (world.main_party_side != 0 && world.enemy_side == 0))
                ? 1
                : 0;
        store_lvalue(dst, native_world_count_team(ctx, team), ctx, locals);
        return NativeDispatch::handled;
    }
    case 2384: { // store_attacker_count (0x1401df702)
        const std::int32_t team =
            (world.main_party_side == 0 ||
             (world.main_party_side != 1 && world.enemy_side == 1))
                ? 1
                : 0;
        store_lvalue(dst, native_world_count_team(ctx, team), ctx, locals);
        return NativeDispatch::handled;
    }
    case 2385: // store_normalized_team_count (0x1401df7d7)
        store_lvalue(dst,
                     world.is_valid_team(narrow[1])
                         ? native_world_count_team(ctx, narrow[1])
                         : 0,
                     ctx, locals);
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_world_3_statement(opcode, wide, narrow, tags, statement,
                                             ctx, locals);
}

}  // namespace wse::vm

#include "wse/vm/native_world_ops_3.h"
