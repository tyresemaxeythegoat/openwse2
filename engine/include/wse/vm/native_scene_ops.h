// Native VM opcodes (Milestone 5): scenes, props, lighting, camera, shaders
// Implemented from WSE2 v1.1.5.6 mbStatement::execute decompilations (56 opcodes).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_ui_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

[[nodiscard]] inline bool native_m5_is_prop_object(
    const game::WseMissionObject& obj) noexcept {
    return !obj.is_item && !obj.m5.is_spawned_item;
}

[[nodiscard]] inline std::int32_t native_m5_count_item_instances(
    const game::WseWorldState& world,
    std::int32_t item_no,
    bool spawned) noexcept {
    std::int32_t count = 0;
    for (const auto& obj : world.mission_objects) {
        if (obj.active && obj.is_item && obj.m5.is_spawned_item == spawned &&
            obj.scene_prop_no == item_no) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] inline std::int32_t native_m5_find_item_instance(
    const game::WseWorldState& world,
    std::int32_t item_no,
    std::int32_t ordinal,
    bool spawned) noexcept {
    if (ordinal < 0) {
        return -1;
    }
    std::int32_t seen = 0;
    for (std::size_t i = 0; i < world.mission_objects.size(); ++i) {
        const auto& obj = world.mission_objects[i];
        if (obj.active && obj.is_item && obj.m5.is_spawned_item == spawned &&
            obj.scene_prop_no == item_no) {
            if (seen == ordinal) {
                return static_cast<std::int32_t>(i);
            }
            ++seen;
        }
    }
    return -1;
}

[[nodiscard]] inline bool native_m5_props_aabb_intersect(
    const game::WseMissionObject& a,
    const game::WseMissionObject& b) noexcept {
    if (!native_m5_is_prop_object(a) || !native_m5_is_prop_object(b) ||
        !a.m5.collision_enabled || !b.m5.collision_enabled) {
        return false;
    }
    const float hx = std::abs(a.m5.bound_half_extents.x * a.scale.x) +
                     std::abs(b.m5.bound_half_extents.x * b.scale.x);
    const float hy = std::abs(a.m5.bound_half_extents.y * a.scale.y) +
                     std::abs(b.m5.bound_half_extents.y * b.scale.y);
    const float hz = std::abs(a.m5.bound_half_extents.z * a.scale.z) +
                     std::abs(b.m5.bound_half_extents.z * b.scale.z);
    return std::abs(a.position.o.x - b.position.o.x) <= hx &&
           std::abs(a.position.o.y - b.position.o.y) <= hy &&
           std::abs(a.position.o.z - b.position.o.z) <= hz;
}

inline void native_m5_upsert_shader_param(game::WseM5WorldExt& m5,
                                          std::string name,
                                          const game::WseShaderParamValue& value) {
    for (auto& kv : m5.shader_params) {
        if (kv.first == name) {
            kv.second = value;
            return;
        }
    }
    m5.shader_params.emplace_back(std::move(name), value);
}

[[nodiscard]] inline std::string native_m5_resolve_underscore_utf8(
    std::int32_t string_index,
    std::uint8_t tag,
    VmContext& ctx) {
    const std::u16string parsed = native_resolve_and_parse(string_index, tag, ctx);
    std::string out = wse_utf16_to_utf8(parsed);
    std::replace(out.begin(), out.end(), ' ', '_');
    return out;
}

inline void native_m5_set_prop_deform_range(game::WseMissionObject& obj,
                                            std::int32_t start_frame,
                                            std::int32_t end_frame,
                                            std::int64_t duration_ms,
                                            bool loop) noexcept {
    obj.m5.deform_start_frame = start_frame;
    obj.m5.deform_end_frame = end_frame;
    obj.m5.deform_duration = static_cast<float>(duration_ms) * 0.001f;
    obj.m5.deform_loop = loop;
    obj.m5.deform_time = static_cast<float>(start_frame);
}

inline void native_m5_start_cam_animation(game::WseMissionCameraState& cam,
                                          const Statement& statement,
                                          std::span<const std::int64_t> wide,
                                          std::span<const std::int32_t> narrow,
                                          std::int32_t dur_op,
                                          std::int32_t nl_op) noexcept {
    const std::int64_t dur_ms = (statement.numOperands > dur_op)
                                    ? wide[static_cast<std::size_t>(dur_op)]
                                    : 0;
    cam.position_duration = static_cast<float>(dur_ms) * 0.001f;
    cam.non_linear = (statement.numOperands > nl_op &&
                      narrow[static_cast<std::size_t>(nl_op)] > 0);
}

[[nodiscard]] inline NativeDispatch dispatch_native_scene_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    auto& world = ctx.world;
    const auto banks = native_string_banks(ctx);
    const float fp = ctx.fixedPointMultiplier != 0
                         ? static_cast<float>(ctx.fixedPointMultiplier)
                         : 1.0f;

    switch (opcode) {
    case 1781: // entry_point_set_position (0x1401d5dcf)
        if (narrow[0] >= 0 && narrow[0] < game::kMaxEntryPoints &&
            wse_register_ok(narrow[1])) {
            auto& ep = world.entry_points[static_cast<std::size_t>(narrow[0])];
            ep.position = ctx.positions[static_cast<std::size_t>(narrow[1])];
            ep.valid = true;
            if (ep.position.o.z < -100.0f) {
                ep.position.o.z = 0.0f;
            }
        }
        return NativeDispatch::handled;
    case 1782: // entry_point_is_auto_generated (0x1401d5f2f)
        if (narrow[0] >= 0 && narrow[0] < game::kMaxEntryPoints) {
            return world.entry_points[static_cast<std::size_t>(narrow[0])]
                           .is_auto_generated
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::condition_false;
    case 1797: // set_rain (0x1401d6696)
        world.m5.rain_type = narrow[0];
        world.m5.rain_strength = static_cast<float>(wide[1]) / 100.0f;
        world.m5.rain_strength_percent = narrow[1];
        if (narrow[0] != 0 && wide[1] > 50) {
            world.m5.precipitation_type = 3;
        }
        return NativeDispatch::handled;
    case 1798: { // set_fog_distance (0x1401d66f7)
        world.m5.fog_distance = static_cast<float>(wide[0]);
        const std::uint32_t col =
            statement.numOperands > 1 ? static_cast<std::uint32_t>(narrow[1]) : 0U;
        world.m5.fog_color = col;
        if (col != 0U && world.lighting.skybox_no >= 0 &&
            world.lighting.skybox_no < 32) {
            world.m5.skybox_fog_colors[static_cast<std::size_t>(
                world.lighting.skybox_no)] = col;
        }
        return NativeDispatch::handled;
    }
    case 1801: // scene_prop_has_agent_on_it (0x1401d6936, callee 0x14013ba40)
        if (world.is_valid_mission_object(narrow[0]) &&
            world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            return (ag.status == 1 && ag.m5.standing_mission_object_no == narrow[0])
                       ? NativeDispatch::handled
                       : NativeDispatch::condition_false;
        }
        return NativeDispatch::condition_false;
    case 1816: // scene_prop_get_max_hit_points (0x1401d6e62)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])]
                        .max_hit_points),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1819: // scene_prop_set_prune_time (0x1401d6f21, callee 0x140153f00)
        // `mbMissionObject::setPruneTime` sets the prune flag (`+0x1d5`, 0x140153f08) when the
        // time is positive, so a prop given a prune time also expires and is removed by
        // `multiplayer_clear_scene` (416). A zero time leaves the flag as it was.
        if (world.is_valid_mission_object(narrow[0]) && wide[1] >= 0) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.m5.prune_time = static_cast<float>(wide[1]);
            if (obj.m5.prune_time > 0.0f) {
                obj.m5.enable_pruning = true;
            }
        }
        return NativeDispatch::handled;
    case 1822: // scene_prop_fade_out (0x1401d6fc6)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.m5.fade_out = true;
            obj.m5.fade_duration = static_cast<float>(wide[1]) / fp;
        }
        return NativeDispatch::handled;
    case 1823: // scene_prop_fade_in (0x1401d705d)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.m5.fade_out = false;
            obj.m5.fade_duration = static_cast<float>(wide[1]) / fp;
        }
        return NativeDispatch::handled;
    case 1830: // scene_item_get_num_instances (0x1401d7744, callee 0x1401371f0)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, native_m5_count_item_instances(world, narrow[1], false),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1831: // scene_item_get_instance (0x1401d7787, callee 0x1401372e0)
        if (world.is_valid_item(narrow[1])) {
            const std::int32_t idx =
                native_m5_find_item_instance(world, narrow[1], narrow[2], false);
            if (idx < 0) {
                return NativeDispatch::condition_false;
            }
            store_lvalue(dst, static_cast<std::int64_t>(idx), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1832: // scene_spawned_item_get_num_instances (0x1401d77dc, callee 0x1401373c0)
        if (world.is_valid_item(narrow[1])) {
            store_lvalue(dst, native_m5_count_item_instances(world, narrow[1], true),
                         ctx, locals);
        }
        return NativeDispatch::handled;
    case 1833: // scene_spawned_item_get_instance (0x1401d781f, callee 0x1401374b0)
        if (world.is_valid_item(narrow[1])) {
            const std::int32_t idx =
                native_m5_find_item_instance(world, narrow[1], narrow[2], true);
            if (idx < 0) {
                return NativeDispatch::condition_false;
            }
            store_lvalue(dst, static_cast<std::int64_t>(idx), ctx, locals);
        }
        return NativeDispatch::handled;
    case 1834: // scene_allows_mounted_units (0x1401d7874)
        if (world.is_valid_site(world.current_site_no) &&
            (world.sites[static_cast<std::size_t>(world.current_site_no)].flags &
             0x800U) != 0U) {
            return NativeDispatch::condition_false;
        }
        return NativeDispatch::handled;
    case 1838: // prop_instance_is_valid (0x1401d795b, callee 0x14013c3a0)
        return world.is_valid_mission_object(narrow[0])
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 1840: // prop_instance_get_variation_id (0x1401d7985)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])]
                        .variation_id_no),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1841: // prop_instance_get_variation_id_2 (0x1401d79cc)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])]
                        .variation_id_2_no),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 1865: // prop_instance_rotate_to_position (0x1401d824f, callee 0x140153f30)
        if (world.is_valid_mission_object(narrow[0]) && wse_register_ok(narrow[1])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            const float dur = static_cast<float>(wide[2]) / 100.0f;
            const float total_rot =
                statement.numOperands > 3 ? static_cast<float>(wide[3]) / 100.0f
                                          : 0.0f;
            if (total_rot == 0.0f) {
                obj.m5.rotation = 0.0f;
                obj.m5.is_rotating = false;
            } else {
                obj.m5.is_rotating = true;
                obj.m5.rotation = obj.m5.total_rotation;
            }
            obj.target_position = ctx.positions[static_cast<std::size_t>(narrow[1])];
            obj.animate_duration = dur;
            obj.m5.animate_rot_duration = dur;
            obj.m5.total_rotation_angle = total_rot;
            obj.animating = true;
        }
        return NativeDispatch::handled;
    case 1866: // prop_instance_initialize_rotation_angles (0x1401d836b)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.m5.total_rotation = 0.0f;
            obj.m5.rotation_angle = 0.0f;
            obj.m5.rotation = 0.0f;
        }
        return NativeDispatch::handled;
    case 1870: // prop_instance_refill_hit_points (0x1401d83a5)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.hit_points = obj.max_hit_points;
        }
        return NativeDispatch::handled;
    case 1871: // prop_instance_dynamics_set_properties (0x1401d83e0)
        if (world.is_valid_mission_object(narrow[0]) && wse_register_ok(narrow[1])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            const auto& o = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            obj.m5.mass = o.x;
            obj.m5.friction = o.y;
        }
        return NativeDispatch::handled;
    case 1872: // prop_instance_dynamics_set_velocity (0x1401d84cd)
        if (world.is_valid_mission_object(narrow[0]) && wse_register_ok(narrow[1])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])]
                .m5.linear_velocity =
                ctx.positions[static_cast<std::size_t>(narrow[1])].o;
        }
        return NativeDispatch::handled;
    case 1873: // prop_instance_dynamics_set_omega (0x1401d858f)
        if (world.is_valid_mission_object(narrow[0]) && wse_register_ok(narrow[1])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])]
                .m5.angular_velocity =
                ctx.positions[static_cast<std::size_t>(narrow[1])].o;
        }
        return NativeDispatch::handled;
    case 1874: // prop_instance_dynamics_apply_impulse (0x1401d8651)
        if (world.is_valid_mission_object(narrow[0]) && wse_register_ok(narrow[1])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])]
                .m5.last_impulse =
                ctx.positions[static_cast<std::size_t>(narrow[1])].o;
        }
        return NativeDispatch::handled;
    case 1877: // prop_instance_receive_damage (0x1401d878a)
        if (world.is_valid_mission_object(narrow[0]) &&
            (narrow[1] == -1 || world.is_valid_agent(narrow[1])) &&
            wide[2] >= 0) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            if (native_m5_is_prop_object(obj) && obj.hit_points > 0.0f) {
                obj.hit_points -= static_cast<float>(wide[2]);
                if (obj.hit_points <= 0.0f) {
                    obj.m5.collision_enabled = false;
                }
            }
        }
        return NativeDispatch::handled;
    case 1880: { // prop_instance_intersects_with_prop_instance (0x1401d8a21)
        if (!world.is_valid_mission_object(narrow[0])) {
            return NativeDispatch::handled;
        }
        const auto& a = world.mission_objects[static_cast<std::size_t>(narrow[0])];
        if (narrow[1] == -1) {
            for (std::size_t i = 0; i < world.mission_objects.size(); ++i) {
                if (static_cast<std::int32_t>(i) == narrow[0] ||
                    !world.mission_objects[i].active) {
                    continue;
                }
                if (native_m5_props_aabb_intersect(a, world.mission_objects[i])) {
                    return NativeDispatch::handled;
                }
            }
            return NativeDispatch::condition_false;
        }
        if (!world.is_valid_mission_object(narrow[1])) {
            return NativeDispatch::handled;
        }
        if (narrow[0] == narrow[1]) {
            return NativeDispatch::condition_false;
        }
        return native_m5_props_aabb_intersect(
                   a, world.mission_objects[static_cast<std::size_t>(narrow[1])])
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }
    case 1881: // prop_instance_play_sound (0x1401d8c76, callee 0x1401544c0)
        if (world.is_valid_mission_object(narrow[0]) && narrow[1] >= 0 &&
            (world.m5.num_sounds <= 0 || narrow[1] < world.m5.num_sounds)) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            const std::int32_t flags = statement.numOperands > 2 ? narrow[2] : 0;
            obj.m5.active_sound_no = narrow[1];
            obj.m5.active_sound_flags = flags;
            obj.m5.sound_playing = true;
            world.played_sounds.push_back(
                game::WseBattleEvent{0, narrow[1], -1, flags, obj.position.o});
            if (world.audio_runtime != nullptr) {
                // One sound per prop instance (`active_sound_no`): replaying stops the
                // previous channel instead of leaking it (looping props would
                // otherwise pile up until the virtual channels run out).
                if (obj.m5.sound_channel >= 0) {
                    world.audio_runtime->stop_channel(obj.m5.sound_channel);
                }
                const std::int32_t ch = world.audio_runtime->play_sound(
                    narrow[1], obj.position.o, static_cast<std::uint32_t>(flags), 1.0f, 0);
                // Stopping the old channel ran the end callback, which cleared the
                // prop's sound fields; set them from the new channel.
                obj.m5.sound_channel = ch;
                obj.m5.sound_playing = (ch >= 0);
                obj.m5.active_sound_no = (ch >= 0) ? narrow[1] : -1;
                obj.m5.active_sound_flags = flags;
                world.music.last_sound_channel = ch;
            }
        }
        return NativeDispatch::handled;
    case 1882: // prop_instance_stop_sound (0x1401d8cc0)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            if (world.audio_runtime != nullptr && obj.m5.sound_channel >= 0) {
                world.audio_runtime->stop_channel(obj.m5.sound_channel);
            }
            obj.m5.sound_channel = -1;
            obj.m5.active_sound_no = -1;
            obj.m5.sound_playing = false;
        }
        return NativeDispatch::handled;
    case 1885: // prop_instance_clear_attached_missiles (0x1401d8d0c)
        if (world.is_valid_mission_object(narrow[0])) {
            const auto& obj =
                world.mission_objects[static_cast<std::size_t>(narrow[0])];
            if (native_m5_is_prop_object(obj)) {
                std::erase_if(world.m5.attached_missiles,
                              [target = narrow[0]](const game::WseAttachedMissile& m) {
                                  return m.mission_object_no == target;
                              });
            }
        }
        return NativeDispatch::handled;
    case 1886: // prop_instance_add_particle_system (0x1401d8ff8)
        if (world.is_valid_mission_object(narrow[0]) &&
            world.is_valid_particle_system(narrow[1]) &&
            wse_register_ok(narrow[2])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])]
                .m5.particle_system_nos.push_back(narrow[1]);
        }
        return NativeDispatch::handled;
    case 1887: // prop_instance_stop_all_particle_systems (0x1401d9089)
        if (world.is_valid_mission_object(narrow[0])) {
            world.mission_objects[static_cast<std::size_t>(narrow[0])]
                .m5.particle_system_nos.clear();
        }
        return NativeDispatch::handled;
    case 1889: // replace_prop_instance (0x1401d90c3)
        if (world.is_valid_mission_object(narrow[0]) &&
            world.is_valid_scene_prop(narrow[1]) &&
            static_cast<std::uint32_t>(ctx.currentCaller + 20) < 2U) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            if (native_m5_is_prop_object(obj)) {
                obj.scene_prop_no = narrow[1];
            }
        }
        return NativeDispatch::handled;
    case 1974: // spawn_scene_prop (0x1401da165, callee 0x14013a5d0)
        // Spawns at the set_spawn_position (1970) transform, like spawn_item
        // and spawn_horse. Safety deviation: capped at kMaxSpawnedMissionObjects.
        if (world.is_valid_scene_prop(narrow[0]) && world.lighting.has_scene) {
            game::WseMissionObject obj{};
            obj.scene_prop_no = narrow[0];
            obj.variation_id_no = statement.numOperands > 1 ? narrow[1] : 0;
            obj.position = world.spawn_position;
            obj.starting_position = world.spawn_position;
            obj.target_position = world.spawn_position;
            const std::int32_t new_id = world.allocate_mission_object(std::move(obj));
            if (new_id >= 0) {
                ctx.registers[0] = static_cast<std::int64_t>(new_id);
            }
        }
        return NativeDispatch::handled;
    case 2001: { // mission_cam_set_mode (0x1401dae26)
        auto& cam = world.m5.mission_camera;
        cam.mode = std::clamp(narrow[0], std::int32_t{0}, std::int32_t{1});
        if (cam.mode == 0 && narrow[1] > 0) {
            cam.target_position = cam.position;
            cam.target_aperture = cam.aperture;
            native_m5_start_cam_animation(cam, statement, wide, narrow, 1, 2);
        } else {
            cam.position_duration = 0.0f;
        }
        return NativeDispatch::handled;
    }
    case 2008: // mission_cam_set_screen_color (0x1401db1b2)
        world.m5.mission_camera.screen_color = static_cast<std::uint32_t>(narrow[0]);
        return NativeDispatch::handled;
    case 2009: { // mission_cam_animate_to_screen_color (0x1401db1c5)
        auto& cam = world.m5.mission_camera;
        cam.screen_color_target = static_cast<std::uint32_t>(narrow[0]);
        cam.screen_color_duration = static_cast<float>(wide[1]) * 0.001f;
        return NativeDispatch::handled;
    }
    case 2010: // mission_cam_get_position (0x1401db216)
        if (wse_register_ok(narrow[0])) {
            ctx.positions[static_cast<std::size_t>(narrow[0])] =
                world.m5.mission_camera.position;
        }
        return NativeDispatch::handled;
    case 2011: // mission_cam_set_position (0x1401db3d7)
        if (wse_register_ok(narrow[0])) {
            auto& cam = world.m5.mission_camera;
            cam.position = ctx.positions[static_cast<std::size_t>(narrow[0])];
            cam.position_duration = 0.0f;
        }
        return NativeDispatch::handled;
    case 2012: // mission_cam_animate_to_position (0x1401db4da)
        if (wse_register_ok(narrow[0]) && wide[1] > 0) {
            auto& cam = world.m5.mission_camera;
            cam.target_position = ctx.positions[static_cast<std::size_t>(narrow[0])];
            cam.target_aperture = cam.aperture;
            cam.animation_no = -1;
            native_m5_start_cam_animation(cam, statement, wide, narrow, 1, 2);
        }
        return NativeDispatch::handled;
    case 2013: // mission_cam_get_aperture (0x1401db804)
        store_lvalue(dst, static_cast<std::int64_t>(world.m5.mission_camera.aperture),
                     ctx, locals);
        return NativeDispatch::handled;
    case 2014: // mission_cam_set_aperture (0x1401db837)
        world.m5.mission_camera.aperture = static_cast<float>(
            std::clamp(narrow[0], std::int32_t{10}, std::int32_t{149}));
        world.m5.mission_camera.position_duration = 0.0f;
        return NativeDispatch::handled;
    case 2015: // mission_cam_animate_to_aperture (0x1401db86c)
        if (wide[1] > 0) {
            auto& cam = world.m5.mission_camera;
            cam.target_position = cam.position;
            cam.target_aperture = static_cast<float>(
                std::clamp(narrow[0], std::int32_t{10}, std::int32_t{149}));
            cam.animation_no = -1;
            native_m5_start_cam_animation(cam, statement, wide, narrow, 1, 2);
        }
        return NativeDispatch::handled;
    case 2016: // mission_cam_animate_to_position_and_aperture (0x1401dbb25)
        if (wse_register_ok(narrow[0]) && wide[2] > 0) {
            auto& cam = world.m5.mission_camera;
            cam.target_position = ctx.positions[static_cast<std::size_t>(narrow[0])];
            cam.target_aperture = static_cast<float>(
                std::clamp(narrow[1], std::int32_t{10}, std::int32_t{149}));
            cam.animation_no = -1;
            native_m5_start_cam_animation(cam, statement, wide, narrow, 2, 3);
        }
        return NativeDispatch::handled;
    case 2017: // mission_cam_set_target_agent (0x1401dbe65)
        if (world.is_valid_agent(narrow[0])) {
            world.m5.mission_camera.target_agent_no = narrow[0];
            world.m5.mission_camera.target_agent_use_rotation =
                (statement.numOperands > 1 && narrow[1] != 0);
        }
        return NativeDispatch::handled;
    case 2018: // mission_cam_clear_target_agent (0x1401dbe95)
        world.m5.mission_camera.target_agent_no = -1;
        return NativeDispatch::handled;
    case 2019: // mission_cam_set_animation (0x1401dbeab)
        world.m5.mission_camera.animation_no = narrow[0];
        return NativeDispatch::handled;
    case 2400: { // set_shader_param_int (0x1401dfd3c)
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (wse_string_register_ok(narrow[0], tag0, banks)) {
            game::WseShaderParamValue v{};
            v.kind = game::WseShaderParamValue::Kind::integer;
            v.int_val = narrow[1];
            native_m5_upsert_shader_param(
                world.m5,
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx)),
                v);
        }
        return NativeDispatch::handled;
    }
    case 2401: { // set_shader_param_float (0x1401dfe3d)
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (wse_string_register_ok(narrow[0], tag0, banks)) {
            game::WseShaderParamValue v{};
            v.kind = game::WseShaderParamValue::Kind::scalar;
            v.float_val = static_cast<float>(wide[1]) / fp;
            native_m5_upsert_shader_param(
                world.m5,
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx)),
                v);
        }
        return NativeDispatch::handled;
    }
    case 2402: { // set_shader_param_float4 (0x1401dff62)
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (wse_string_register_ok(narrow[0], tag0, banks)) {
            game::WseShaderParamValue v{};
            v.kind = game::WseShaderParamValue::Kind::vec4;
            for (std::size_t i = 0; i < 4; ++i) {
                v.matrix_val[i] = static_cast<float>(wide[1 + i]) / fp;
            }
            native_m5_upsert_shader_param(
                world.m5,
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx)),
                v);
        }
        return NativeDispatch::handled;
    }
    case 2403: { // set_shader_param_float4x4 (0x1401e012a)
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        if (wse_string_register_ok(narrow[0], tag0, banks)) {
            game::WseShaderParamValue v{};
            v.kind = game::WseShaderParamValue::Kind::mat4;
            for (std::size_t r = 0; r < 4; ++r) {
                v.matrix_val[r * 4 + 0] = static_cast<float>(wide[1 + r * 3 + 0]) / fp;
                v.matrix_val[r * 4 + 1] = static_cast<float>(wide[1 + r * 3 + 1]) / fp;
                v.matrix_val[r * 4 + 2] = static_cast<float>(wide[1 + r * 3 + 2]) / fp;
                v.matrix_val[r * 4 + 3] = (r == 3) ? 1.0f : 0.0f;
            }
            native_m5_upsert_shader_param(
                world.m5,
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx)),
                v);
        }
        return NativeDispatch::handled;
    }
    case 2610: // prop_instance_deform_to_time (0x1401e03ec)
        if (world.is_valid_mission_object(narrow[0])) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.m5.deform_time = static_cast<float>(wide[1]);
            obj.m5.deform_progress = 0.0f;
        }
        return NativeDispatch::handled;
    case 2611: // prop_instance_deform_in_range (0x1401e04dc)
        if (world.is_valid_mission_object(narrow[0])) {
            native_m5_set_prop_deform_range(
                world.mission_objects[static_cast<std::size_t>(narrow[0])], narrow[1],
                narrow[2], wide[3], false);
        }
        return NativeDispatch::handled;
    case 2612: // prop_instance_deform_in_cycle_loop (0x1401e05ee)
        if (world.is_valid_mission_object(narrow[0])) {
            native_m5_set_prop_deform_range(
                world.mission_objects[static_cast<std::size_t>(narrow[0])], narrow[1],
                narrow[2], wide[3], true);
        }
        return NativeDispatch::handled;
    case 2615: // prop_instance_get_current_deform_progress (0x1401e06fa)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])]
                        .m5.deform_progress *
                    100.0f),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2616: // prop_instance_get_current_deform_frame (0x1401e07fa)
        if (world.is_valid_mission_object(narrow[1])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(std::llround(static_cast<double>(
                    world.mission_objects[static_cast<std::size_t>(narrow[1])]
                        .m5.deform_time))),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2617: { // prop_instance_set_material (0x1401e08f0)
        const std::uint8_t tag2 = wse_tag_at(tags, 2);
        if (world.is_valid_mission_object(narrow[0]) &&
            wse_string_register_ok(narrow[2], tag2, banks)) {
            auto& obj = world.mission_objects[static_cast<std::size_t>(narrow[0])];
            obj.m5.material_submesh = narrow[1];
            obj.m5.material_name =
                native_m5_resolve_underscore_utf8(narrow[2], tag2, ctx);
        }
        return NativeDispatch::handled;
    }
    default:
        break;
    }

    return dispatch_native_ui_statement(opcode, wide, narrow, tags, statement, ctx,
                                        locals);
}

}  // namespace wse::vm

#include "wse/vm/native_ui_ops.h"
