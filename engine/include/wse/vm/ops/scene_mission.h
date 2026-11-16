// Milestone 6 scene, mission, missile, prop, and ground alignment opcode handlers
// (44 WSE opcodes: 3602..3607, 3609, 3611..3618, 3620, 3621, 3623..3625, 4101,
// 4500..4522).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "wse/core/math.h"
#include "wse/vm/wse_state.h"

namespace wse::vm {

struct WseRayCapsuleHit {
    bool hit = false;
    float distance = 0.0f;
    WseVec4 point{};
    WseVec4 normal{};
};

[[nodiscard]] inline const wse::game::WseM6MissionObjectExt*
wse_m6_find_mission_object(const wse::game::WseWorldState& world,
                           std::int32_t prop_no) noexcept {
    const auto it = world.m6.mission_objects.find(prop_no);
    return (it != world.m6.mission_objects.end()) ? &it->second : nullptr;
}

[[nodiscard]] inline WseVec4 wse_m6_safe_normalize(const WseVec4& v,
                                                   const WseVec4& fallback) noexcept {
    const float len = v.length();
    if (len > 1.0e-6f) {
        const float inv = 1.0f / len;
        return WseVec4{v.x * inv, v.y * inv, v.z * inv, 0.0f};
    }
    return fallback;
}

[[nodiscard]] inline WseRayCapsuleHit wse_m6_ray_capsule_intersect(
    const WseVec4& ray_origin,
    const WseVec4& ray_dir_unit,
    float ray_len,
    const WseVec4& cap_a,
    const WseVec4& cap_b,
    float radius) noexcept {
    WseRayCapsuleHit best{};
    const WseVec4 ab = cap_b - cap_a;
    const float nn = ab.dot(ab);
    const WseVec4 m = ray_origin - cap_a;

    // Check if the ray origin already starts inside the capsule.
    float s0 = 0.0f;
    if (nn > 1.0e-8f) {
        s0 = std::clamp(m.dot(ab) / nn, 0.0f, 1.0f);
    }
    const WseVec4 closest_start = cap_a + ab * s0;
    const WseVec4 diff_start = ray_origin - closest_start;
    if (diff_start.dot(diff_start) <= radius * radius) {
        best.hit = true;
        best.distance = 0.0f;
        best.point = ray_origin;
        best.normal = wse_m6_safe_normalize(
            diff_start,
            WseVec4{-ray_dir_unit.x, -ray_dir_unit.y, -ray_dir_unit.z, 0.0f});
        return best;
    }

    float best_t = ray_len + 1.0f;
    auto consider_hit = [&](float t, const WseVec4& axis_point) {
        if (t < 0.0f || t > ray_len || t >= best_t) {
            return;
        }
        best_t = t;
        best.hit = true;
        best.distance = t;
        best.point = ray_origin + ray_dir_unit * t;
        best.normal = wse_m6_safe_normalize(
            best.point - axis_point,
            WseVec4{-ray_dir_unit.x, -ray_dir_unit.y, -ray_dir_unit.z, 0.0f});
    };

    if (nn > 1.0e-8f) {
        const float nd = ab.dot(ray_dir_unit);
        const float mn = m.dot(ab);
        const float md = m.dot(ray_dir_unit);
        const float a = nn - nd * nd;
        if (a > 1.0e-8f) {
            const float k = m.dot(m) - radius * radius;
            const float b = nn * md - nd * mn;
            const float c = nn * k - mn * mn;
            const float discr = b * b - a * c;
            if (discr >= 0.0f) {
                const float t = (-b - std::sqrt(discr)) / a;
                const float m_proj = mn + t * nd;
                if (m_proj >= 0.0f && m_proj <= nn) {
                    consider_hit(t, cap_a + ab * (m_proj / nn));
                }
            }
        }
    }

    auto test_sphere = [&](const WseVec4& center) {
        const WseVec4 oc = ray_origin - center;
        const float b = oc.dot(ray_dir_unit);
        const float c = oc.dot(oc) - radius * radius;
        const float discr = b * b - c;
        if (discr >= 0.0f) {
            const float t = -b - std::sqrt(discr);
            consider_hit(t, center);
        }
    };

    test_sphere(cap_a);
    test_sphere(cap_b);
    return best;
}

template <typename Fn>
[[nodiscard]] inline WseDispatch wse_m6_mutate_site_terrain(
    wse::game::WseWorldState* world,
    std::int32_t scene_no,
    Fn&& fn) {
    if (world == nullptr || !world->is_valid_site(scene_no)) {
        return WseDispatch::ignored;
    }
    fn(world->m6.site(scene_no));
    return WseDispatch::handled_no_store;
}

[[nodiscard]] inline WseDispatch execute_wse_scene_mission(
    std::uint32_t opcode,
    const WseOperands& operands,
    WseEffect& effect) {
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    auto* world = operands.state.world;
    const auto positions = operands.state.positions;
    const float fp_mul =
        static_cast<float>(operands.state.fixed_point_multiplier != 0
                               ? operands.state.fixed_point_multiplier
                               : 100);

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::set_ally_collision_threshold: {
        // 3602 (0x1401ea751)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 || narrow[0] > 100 ||
            narrow[1] < 0 || narrow[1] > 100) {
            return WseDispatch::ignored;
        }
        world->m6.scene.ally_collision_min =
            static_cast<float>(narrow[0]) / 100.0f;
        world->m6.scene.ally_collision_max =
            static_cast<float>(narrow[1]) / 100.0f;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::particle_system_remove: {
        // 3603 (0x1401ea7be)
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        const std::int32_t ps_no = !narrow.empty() ? narrow[0] : -1;
        world->m6.scene.removed_particle_systems.push_back(ps_no);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_spectated_agent_no: {
        // 3604 (0x1401ea7fa)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value = world->m6.scene.spectated_agent_no;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::prop_instance_set_forced_lod: {
        // 3605 (0x1401ea826)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_mission_object(narrow[0]) ||
            narrow[1] < 0 || narrow[1] >= 6) {
            return WseDispatch::ignored;
        }
        world->m6.mission_object(narrow[0]).forced_lod = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::prop_instance_set_variation_id: {
        // 3606 (0x1401ea880)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_mission_object(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->mission_objects[static_cast<std::size_t>(narrow[0])]
            .variation_id_no = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::prop_instance_set_variation_id_2: {
        // 3607 (0x1401ea8be)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_mission_object(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->mission_objects[static_cast<std::size_t>(narrow[0])]
            .variation_id_2_no = narrow[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::missile_get_path_point_position: {
        // 3609 (0x1401ea9ff)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= positions.size() ||
            narrow[1] < 0 || narrow[1] >= 500 ||
            !world->m6.is_valid_missile(narrow[2])) {
            return WseDispatch::ignored;
        }
        const auto& pts = world->m6.missiles.at(narrow[2]).path_points;
        const auto pt_idx = static_cast<std::size_t>(narrow[1]);
        if (pt_idx >= pts.size()) {
            return WseDispatch::ignored;
        }
        const auto& pt = pts[pt_idx];
        WseTransform out = WseTransform::identity();
        const WseVec4 raw_f = (pt_idx == 0) ? pt.f : (pt.o - pts[pt_idx - 1].o);
        out.f = wse_m6_safe_normalize(raw_f, WseVec4{0.0f, 1.0f, 0.0f, 0.0f});
        out.u = WseVec4{0.0f, 0.0f, 1.0f, 0.0f};
        out.o = pt.o;
        out.orthonormalize();
        positions[static_cast<std::size_t>(narrow[0])] = out;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::missile_remove_on_hit: {
        // 3611 (0x1401eabfa): active during ti_on_missile_hit (-52).
        if (world == nullptr || operands.state.caller != -52) {
            return WseDispatch::ignored;
        }
        // Only the missile that fired the trigger is flagged.
        if (!world->m6.is_valid_missile(world->m6.trigger_missile_no)) {
            return WseDispatch::ignored;
        }
        world->m6.missiles[world->m6.trigger_missile_no].remove_on_hit = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::missile_is_valid: {
        // 3612 (0x1401eac26)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::condition_false;
        }
        return world->m6.is_valid_missile(narrow[0])
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::missile_get_cur_position: {
        // 3613 (0x1401eac47)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= positions.size() ||
            !world->m6.is_valid_missile(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& pts = world->m6.missiles.at(narrow[1]).path_points;
        WseTransform out = WseTransform::identity();
        if (!pts.empty()) {
            const auto& last = pts.back();
            const WseVec4 raw_f =
                (pts.size() < 2) ? last.f
                                 : (last.o - pts[pts.size() - 2].o);
            out.f = wse_m6_safe_normalize(raw_f, WseVec4{0.0f, 1.0f, 0.0f, 0.0f});
            out.u = WseVec4{0.0f, 0.0f, 1.0f, 0.0f};
            out.o = last.o;
            out.orthonormalize();
        }
        positions[static_cast<std::size_t>(narrow[0])] = out;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::set_prop_collision_threshold: {
        // 3614 (0x1401eae2c)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 || narrow[0] >= 4 ||
            narrow[1] < 0 || narrow[1] > 100 || narrow[2] < 0 ||
            narrow[2] > 100) {
            return WseDispatch::ignored;
        }
        auto& thresh =
            world->m6.scene
                .prop_collision_thresholds[static_cast<std::size_t>(narrow[0])];
        thresh[0] = static_cast<float>(narrow[1]) / 100.0f;
        thresh[1] = static_cast<float>(narrow[2]) / 100.0f;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::get_camera_position: {
        // 3615 (0x1401eaeb1)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        positions[static_cast<std::size_t>(narrow[0])] =
            world->misc.camera_transform;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::prop_instance_remove_particle_system: {
        // 3616 (0x1401eaef1)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_mission_object(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::int32_t ps_no = (narrow.size() >= 2) ? narrow[1] : -1;
        auto& list = world->mission_objects[static_cast<std::size_t>(narrow[0])]
                         .m5.particle_system_nos;
        if (ps_no < 0) {
            list.clear();
        } else {
            list.erase(std::remove(list.begin(), list.end(), ps_no), list.end());
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::prop_instance_remove_light: {
        // 3617 (0x1401eb021)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_mission_object(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->m6.mission_object(narrow[0]).has_light = false;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::prop_instance_get_sound_progress: {
        // 3618 (0x1401eb086)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_mission_object(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& mo =
            world->mission_objects[static_cast<std::size_t>(narrow[1])];
        if (world->audio_runtime != nullptr && mo.m5.sound_channel >= 0) {
            effect.value =
                world->audio_runtime->get_sound_progress(mo.m5.sound_channel);
        } else if (mo.m5.active_sound_no < 0 && !mo.m5.sound_playing) {
            effect.value = -1;
        } else {
            const auto* mox = wse_m6_find_mission_object(*world, narrow[1]);
            const float prog = mox ? mox->sound_progress : 0.0f;
            effect.value = std::clamp(static_cast<std::int64_t>(prog * 100.0f),
                                      std::int64_t{0}, std::int64_t{100});
        }
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::cast_ray_agents: {
        // 3620 (0x1401eb5f4)
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) ||
            static_cast<std::size_t>(narrow[1]) >= positions.size() ||
            static_cast<std::size_t>(narrow[2]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        const float ray_len =
            (wide.size() >= 4 && wide[3] != 0)
                ? static_cast<float>(wide[3]) / fp_mul
                : 10000.0f;
        if (ray_len < 0.0f) {
            return WseDispatch::condition_false;
        }

        const auto& ray_pos = positions[static_cast<std::size_t>(narrow[2])];
        const WseVec4 ray_dir =
            wse_m6_safe_normalize(ray_pos.f, WseVec4{0.0f, 1.0f, 0.0f, 0.0f});

        std::int32_t best_agent = -1;
        WseRayCapsuleHit best_hit{};
        for (std::size_t idx = 0; idx < world->agents.size(); ++idx) {
            const auto& ag = world->agents[idx];
            if (ag.status != 1) {
                continue;
            }
            const bool human = ag.is_human();
            const auto ag_it =
                world->m6.agents.find(static_cast<std::int32_t>(idx));
            const float raw_scale =
                (ag_it != world->m6.agents.end()) ? ag_it->second.scale : 1.0f;
            const float scale = (raw_scale > 0.0f) ? raw_scale : 1.0f;
            const float radius = (human ? 0.35f : 0.65f) * scale;
            const float height = (human ? 1.85f : 1.70f) * scale;
            const WseVec4 cap_a{ag.position.o.x, ag.position.o.y,
                                ag.position.o.z + radius, 1.0f};
            const WseVec4 cap_b{ag.position.o.x, ag.position.o.y,
                                ag.position.o.z + std::max(radius, height - radius),
                                1.0f};
            const WseRayCapsuleHit hit = wse_m6_ray_capsule_intersect(
                ray_pos.o, ray_dir, ray_len, cap_a, cap_b, radius);
            if (!hit.hit) {
                continue;
            }
            if (!best_hit.hit || hit.distance < best_hit.distance - 1.0e-5f ||
                (std::fabs(hit.distance - best_hit.distance) <= 1.0e-5f &&
                 !human)) {
                best_hit = hit;
                best_agent = static_cast<std::int32_t>(idx);
            }
        }

        if (best_agent < 0) {
            return WseDispatch::condition_false;
        }
        if (!operands.registers.empty()) {
            operands.registers[0] = 0;
        }
        auto& out_pos = positions[static_cast<std::size_t>(narrow[1])];
        out_pos = WseTransform::identity();
        out_pos.o = best_hit.point;
        out_pos.o.w = 1.0f;
        out_pos.f = best_hit.normal;
        out_pos.u = WseVec4{0.0f, 0.0f, 1.0f, 0.0f};
        out_pos.orthonormalize();
        effect.value = best_agent;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::ai_mesh_face_group_translate: {
        // 3621 (0x1401ebb0a)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !wse_register_ok(narrow[1]) ||
            static_cast<std::size_t>(narrow[1]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        const auto& delta = positions[static_cast<std::size_t>(narrow[1])].o;
        auto& acc = world->m6.scene.ai_mesh_group_translations[narrow[0]];
        acc.x += delta.x;
        acc.y += delta.y;
        acc.z += delta.z;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::shift_entry_point: {
        // 3623 (0x1401ebc9c)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || narrow[0] < 0 ||
            narrow[0] >= wse::game::kMaxEntryPoints) {
            return WseDispatch::ignored;
        }
        world->entry_points[static_cast<std::size_t>(narrow[0])]
            .position.o.x += 1.0f;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::missile_remove: {
        // 3624 (0x1401ebcdf)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->m6.is_valid_missile(narrow[0])) {
            return WseDispatch::ignored;
        }
        world->m6.missiles[narrow[0]].removed = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_prop_remove: {
        // 3625 (0x1401ebd53)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_mission_object(narrow[0])) {
            return WseDispatch::ignored;
        }
        if (world->is_multiplayer && !world->is_server) {
            return WseDispatch::ignored;
        }
        auto& mo = world->mission_objects[static_cast<std::size_t>(narrow[0])];
        if (mo.is_item) {
            return WseDispatch::ignored;
        }
        mo.active = false;
        world->m6.mission_object(narrow[0]).removed = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::position_align_to_ground: {
        // 4101 (0x1401ecf37)
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        const std::int32_t point_up = (narrow.size() >= 2) ? narrow[1] : 0;
        const std::int32_t set_z = (narrow.size() >= 3) ? narrow[2] : 0;
        const WseVec4 ground_normal{0.0f, 0.0f, 1.0f, 0.0f};
        auto& pos = positions[static_cast<std::size_t>(narrow[0])];
        if (point_up == 0) {
            pos.f = WseVec4{0.0f, 1.0f, 0.0f, 0.0f};
            pos.u = ground_normal;
            pos.orthonormalize();
        } else {
            pos.f = ground_normal;
            pos.u = WseVec4{0.0f, 0.0f, 1.0f, 0.0f};
            pos.orthonormalize();
        }
        if (set_z != 0 && world != nullptr) {
            pos.o.z = world->ground_level_z;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_flags: {
        // 4500 (0x1401f0dd3)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_site(narrow[0])) {
            return WseDispatch::ignored;
        }
        const auto idx = static_cast<std::size_t>(narrow[0]);
        using SiteFlagsT = decltype(world->sites[idx].flags);
        using M6FlagsT = decltype(world->m6.site(narrow[0]).flags);
        world->sites[idx].flags = static_cast<SiteFlagsT>(narrow[1]);
        world->m6.site(narrow[0]).flags = static_cast<M6FlagsT>(narrow[1]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_water_level: {
        // 4501 (0x1401f0e02)
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_site(narrow[0])) {
            return WseDispatch::ignored;
        }
        const float wl = static_cast<float>(wide[1]) / fp_mul;
        world->sites[static_cast<std::size_t>(narrow[0])].water_level = wl;
        world->m6.site(narrow[0]).water_level = wl;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_bounds: {
        // 4502 (0x1401f0e48)
        if (narrow.size() < 5 || wide.size() < 5) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr || !world->is_valid_site(narrow[0])) {
            return WseDispatch::ignored;
        }
        auto& sx = world->m6.site(narrow[0]);
        sx.min_bound = {static_cast<float>(wide[1]) / fp_mul,
                        static_cast<float>(wide[2]) / fp_mul};
        sx.max_bound = {static_cast<float>(wide[3]) / fp_mul,
                        static_cast<float>(wide[4]) / fp_mul};
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_outer_terrain: {
        // 4503 (0x1401f0ee9)
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string parsed;
        if (world == nullptr || !world->is_valid_site(narrow[0]) ||
            !wse_resolve_parsed_utf8_operand(1, operands, parsed)) {
            return WseDispatch::ignored;
        }
        world->m6.site(narrow[0]).outer_terrain = std::move(parsed);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_terrain_seed: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_seed(0, narrow[1]);
        });
    }
    case WseOpcode::scene_set_river_seed: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_seed(1, narrow[1]);
        });
    }
    case WseOpcode::scene_set_flora_seed: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_seed(2, narrow[1]);
        });
    }
    case WseOpcode::scene_set_deep_water: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_deep_water(narrow[1] != 0);
        });
    }
    case WseOpcode::scene_set_place_river: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_place_river(narrow[1] != 0);
        });
    }
    case WseOpcode::scene_set_disable_grass: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_disable_grass(narrow[1] != 0);
        });
    }
    case WseOpcode::scene_set_valley_size: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_valley_size(static_cast<float>(narrow[1]) / 100.0f);
        });
    }
    case WseOpcode::scene_set_hill_height: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_hill_height(static_cast<float>(narrow[1]));
        });
    }
    case WseOpcode::scene_set_ruggedness: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_ruggedness(narrow[1]);
        });
    }
    case WseOpcode::scene_set_vegetation: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_vegetation(static_cast<float>(narrow[1]) / 100.0f);
        });
    }
    case WseOpcode::scene_set_size: {
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_size(static_cast<float>(narrow[1]),
                        static_cast<float>(narrow[2]));
        });
    }
    case WseOpcode::scene_set_region_type: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_region_type(narrow[1]);
        });
    }
    case WseOpcode::scene_set_region_detail: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_region_detail(narrow[1]);
        });
    }
    case WseOpcode::scene_set_region_variant: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_m6_mutate_site_terrain(world, narrow[0], [&](auto& sx) {
            sx.set_region_variant(narrow[1]);
        });
    }
    case WseOpcode::scene_set_height_fog: {
        // 4518 (0x1401f16f4)
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        // Safety deviation: saturating float -> int32 conversion.
        world->m6.scene.height_fog_height =
            static_cast<std::int32_t>(std::clamp<std::int64_t>(
                wse::game::native_f2i_saturate(static_cast<float>(wide[0]) /
                                               fp_mul),
                std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max()));
        world->m6.scene.height_fog_density =
            std::max(static_cast<float>(wide[1]) / fp_mul, 0.1f);
        world->m6.scene.height_fog_strength =
            std::clamp(static_cast<float>(wide[2]) / fp_mul, 0.0f, 20.0f);
        world->m6.scene.height_fog_enabled =
            (world->m6.scene.height_fog_strength > 0.0f);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_water_waves: {
        // 4519 (0x1401f17a0)
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.scene.water_wave_amplitude =
            std::clamp(static_cast<float>(wide[0]) / fp_mul, 0.0f, 5.0f);
        world->m6.scene.water_wave_scale =
            std::clamp(static_cast<float>(wide[1]) / fp_mul, 1.0f, 100.0f);
        world->m6.scene.water_wave_speed =
            std::clamp(static_cast<float>(wide[2]) / fp_mul, 0.0f, 10.0f);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_flora_wind: {
        // 4520 (0x1401f185c)
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.scene.flora_wind_amplitude =
            std::clamp(static_cast<float>(wide[0]) / fp_mul, 0.0f, 2.0f);
        world->m6.scene.flora_wind_wavelength =
            std::clamp(static_cast<float>(wide[1]) / fp_mul, 1.0f, 200.0f);
        world->m6.scene.flora_wind_speed =
            std::clamp(static_cast<float>(wide[2]) / fp_mul, 0.0f, 50.0f);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_rebake_sky_cube: {
        // 4521 (0x1401f1911)
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.scene.sky_cube_rebake_requested = true;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::scene_set_weather_direction: {
        // 4522 (0x1401f1937)
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (world == nullptr) {
            return WseDispatch::ignored;
        }
        world->m6.scene.weather_direction =
            (wide[0] < 0) ? -1000.0f : static_cast<float>(wide[0]);
        return WseDispatch::handled_no_store;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
