// Extended native Warband opcode handlers (world, face keys, input, notes,
// items, parties, troops). Grounded in official WSE2 v1.1.5.6 PDB/binary
// decompilation:
//   undocumented (53, 2180, 2749)          0x1401c671c, 0x1401ddca9, 0x1401e14f3
//   input (65, 70..73, 77)                 0x1401c68d2..0x1401c6b35
//   positions_and_math (707, 750, 1628..)  0x1401cadcc, 0x1401cbdbc, 0x1401d20f9
//   parties & troops (110..2388)           0x1401c6e51..0x1401df900
//   strings & items (1964..2747)           0x1401d9b19..0x1401e13d8
//   face_keys (2752..2767)                 0x1401e17a1..0x1401e22d0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "wse/vm/block_vm.h"

namespace wse::vm {

[[nodiscard]] inline NativeDispatch dispatch_native_world_2_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

struct NativeRayHitResult {
    bool hit = false;
    float t = 0.0f;
    std::int32_t object_no = -1;
    bool is_prop = false;
    wse::core::WseVec4 point{};
    wse::core::WseVec4 normal{0.0f, 0.0f, 1.0f, 0.0f};
};

// Shared slab segment-vs-AABB intersection for 707 (line of sight) and 1900 (cast_ray).
[[nodiscard]] inline NativeRayHitResult native_world_ray_aabb_hit(
    const game::WseWorldState& world,
    const wse::core::WseVec4& origin,
    const wse::core::WseVec4& dir,
    float max_dist,
    std::uint32_t mask) noexcept {
    NativeRayHitResult best{};
    if (!world.lighting.has_scene || max_dist <= 0.0f) {
        return best;
    }
    float best_t = max_dist;
    const float ox = origin.x;
    const float oy = origin.y;
    const float oz = origin.z;
    const float dx = dir.x;
    const float dy = dir.y;
    const float dz = dir.z;

    for (const auto& obs : world.fx.ray_obstacles) {
        if (mask != 0xffffffffu && (obs.mask & mask) == 0U) {
            continue;
        }
        float tmin = 0.0f;
        float tmax = best_t;
        wse::core::WseVec4 hit_n{0.0f, 0.0f, 1.0f, 0.0f};

        const float mins[3] = {obs.min_bound.x, obs.min_bound.y, obs.min_bound.z};
        const float maxs[3] = {obs.max_bound.x, obs.max_bound.y, obs.max_bound.z};
        const float orig[3] = {ox, oy, oz};
        const float dvec[3] = {dx, dy, dz};

        bool miss = false;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::fabs(dvec[axis]) < 1e-8f) {
                if (orig[axis] < mins[axis] || orig[axis] > maxs[axis]) {
                    miss = true;
                    break;
                }
                continue;
            }
            const float inv = 1.0f / dvec[axis];
            float t1 = (mins[axis] - orig[axis]) * inv;
            float t2 = (maxs[axis] - orig[axis]) * inv;
            float sign = -1.0f;
            if (t1 > t2) {
                std::swap(t1, t2);
                sign = 1.0f;
            }
            if (t1 > tmin) {
                tmin = t1;
                hit_n = wse::core::WseVec4{axis == 0 ? sign : 0.0f,
                                           axis == 1 ? sign : 0.0f,
                                           axis == 2 ? sign : 0.0f,
                                           0.0f};
            }
            tmax = std::min(tmax, t2);
            if (tmin > tmax) {
                miss = true;
                break;
            }
        }
        if (!miss && tmin >= 0.0f && tmin < best_t) {
            best_t = tmin;
            best.hit = true;
            best.t = tmin;
            best.is_prop = obs.is_prop;
            best.object_no = obs.object_no;
            best.point = wse::core::WseVec4{ox + dx * tmin, oy + dy * tmin, oz + dz * tmin, 1.0f};
            best.normal = hit_n;
        }
    }
    return best;
}

[[nodiscard]] inline game::WseFaceKeys native_world_parse_face_keys(std::int32_t idx,
                                                                    std::uint8_t tag,
                                                                    VmContext& ctx) {
    game::WseFaceKeys fk{};
    const std::u16string parsed = native_resolve_and_parse(idx, tag, ctx);
    const std::string utf8 = wse_utf16_to_utf8(parsed);
    if (utf8.size() == 64U) {
        (void)fk.from_string(utf8);
    }
    return fk;
}

inline void native_world_fk_get6(std::uint64_t dst,
                                 std::int32_t str_idx,
                                 std::uint8_t tag,
                                 std::uint32_t shift,
                                 VmContext& ctx,
                                 std::span<std::int64_t> locals) {
    if (!wse_string_register_ok(str_idx, tag, native_string_banks(ctx))) {
        return;
    }
    const game::WseFaceKeys fk = native_world_parse_face_keys(str_idx, tag, ctx);
    store_lvalue(dst, static_cast<std::int64_t>(fk.get_bits6(shift)), ctx, locals);
}

inline void native_world_fk_set6(std::int32_t reg_idx,
                                 std::uint8_t tag,
                                 std::int32_t value,
                                 std::uint32_t shift,
                                 VmContext& ctx) {
    if (!wse_register_ok(reg_idx)) {
        return;
    }
    game::WseFaceKeys fk = native_world_parse_face_keys(reg_idx, tag, ctx);
    fk.set_bits6(shift, value);
    ctx.stringRegisters[static_cast<std::size_t>(reg_idx)] = wse_utf8_to_utf16(fk.to_string());
}

template <std::size_t N>
[[nodiscard]] inline NativeDispatch native_world_check_key_state(
    std::int32_t key,
    bool allow_in_edit_mode,
    const std::array<bool, N>& states,
    const game::WseWorldState& world) noexcept {
    if (key < 0 || static_cast<std::size_t>(key) >= N) {
        return NativeDispatch::condition_false;
    }
    if (!allow_in_edit_mode && world.edit_mode.active) {
        return NativeDispatch::condition_false;
    }
    return states[static_cast<std::size_t>(key)] ? NativeDispatch::handled
                                                 : NativeDispatch::condition_false;
}

[[nodiscard]] inline std::u16string native_world_dialog_note_text(
    const game::WseWorldState& world) {
    std::string out;
    if (world.is_valid_troop(world.dialog.speaker_troop_no)) {
        out = world.troops[static_cast<std::size_t>(world.dialog.speaker_troop_no)].name;
    }
    out += ": ";
    out += world.dialog.speaker_text;
    return wse_utf8_to_utf16(out);
}

template <typename NoteT>
inline void native_world_assign_dialog_note(
    std::array<NoteT, game::kNumQuestNotes>& notes,
    std::int32_t slot,
    bool show_date,
    const game::WseWorldState& world) {
    if (slot < 0 || slot >= game::kNumQuestNotes) {
        return;
    }
    auto& note = notes[static_cast<std::size_t>(slot)];
    note.text = native_world_dialog_note_text(world);
    note.date = show_date ? world.current_day : -1;
}

template <typename NoteT>
inline void native_world_assign_sreg_note(
    std::array<NoteT, game::kNumQuestNotes>& notes,
    std::int32_t slot,
    std::int32_t str_idx,
    std::uint8_t tag,
    bool show_date,
    VmContext& ctx) {
    if (slot < 0 || slot >= game::kNumQuestNotes ||
        !wse_string_register_ok(str_idx, tag, native_string_banks(ctx))) {
        return;
    }
    auto& note = notes[static_cast<std::size_t>(slot)];
    note.text = native_resolve_and_parse(str_idx, tag, ctx);
    note.date = show_date ? ctx.world.current_day : -1;
}

inline void native_world_spaces_to_underscores(std::string& text) noexcept {
    for (char& ch : text) {
        if (ch == ' ') {
            ch = '_';
        }
    }
}

[[nodiscard]] inline NativeDispatch dispatch_native_world_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    const auto dst = static_cast<std::uint64_t>(statement.operands[0]);
    auto& world = ctx.world;
    const float fp = static_cast<float>(ctx.fixedPointMultiplier);
    const std::uint8_t tag0 = tags.size() > 0 ? tags[0] : 0U;
    const std::uint8_t tag1 = tags.size() > 1 ? tags[1] : 0U;
    const std::uint8_t tag2 = tags.size() > 2 ? tags[2] : 0U;

    switch (opcode) {
    // -- undocumented (3): 53, 2180, 2749 --
    case 53: // is_cheat_mode_enabled (0x1401c671c)
        return ctx.options.cheat_mode ? NativeDispatch::handled
                                      : NativeDispatch::condition_false;
    case 2180: { // store_main_party_wounded (0x1401ddca9)
        std::int32_t wounded = 0;
        if (world.is_valid_party(world.main_party_no)) {
            const std::int32_t ironflesh_cap =
                world.skills[static_cast<std::size_t>(game::kSkillIronflesh)].max_level;
            const auto& main_p = world.parties[static_cast<std::size_t>(world.main_party_no)];
            for (const auto& st : main_p.stacks) {
                if (st.is_prisoner() || !world.is_valid_troop(st.troop_no)) {
                    continue;
                }
                const auto& tr = world.troops[static_cast<std::size_t>(st.troop_no)];
                if (tr.is_hero()) {
                    const float max_hp = static_cast<float>(tr.max_hit_points(ironflesh_cap));
                    const float delta = (max_hp - tr.hit_points) * 0.15f;
                    wounded += static_cast<std::int32_t>(std::ceil(delta));
                } else {
                    wounded += st.num_wounded_troops;
                }
            }
        }
        store_lvalue(dst, static_cast<std::int64_t>(wounded), ctx, locals);
        return NativeDispatch::handled;
    }
    case 2749: { // op_2749 (0x1401e14f3): str_store_agent_face_keys (sreg, agent_id)
        if (wse_register_ok(narrow[0]) && world.is_valid_agent(narrow[1])) {
            const auto& ag = world.agents[static_cast<std::size_t>(narrow[1])];
            game::WseFaceKeys fk = ag.face_keys;
            const bool zero_keys =
                (fk.keys[0] == 0ULL && fk.keys[1] == 0ULL && fk.keys[2] == 0ULL && fk.keys[3] == 0ULL);
            if (zero_keys && world.is_valid_troop(ag.troop_no)) {
                const auto& tr = world.troops[static_cast<std::size_t>(ag.troop_no)];
                fk.keys = tr.face_keys[0];
                if (ag.dna >= 0) {
                    const std::uint64_t dna_mix =
                        static_cast<std::uint64_t>(static_cast<std::uint32_t>(ag.dna)) *
                        0x9e3779b97f4a7c15ULL;
                    fk.keys[1] ^= (tr.face_keys[1][1] ^ dna_mix) & 0x0707070707070707ULL;
                }
            }
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(fk.to_string());
        }
        return NativeDispatch::handled;
    }

    // -- input (6): 65, 70, 71, 72, 73, 77 --
    case 65: // game_key_get_mapped_key_name (0x1401c68d2)
        if (wse_register_ok(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kMaxGameKeys && narrow[2] >= 0 && narrow[2] < 2) {
            const std::string& name =
                (narrow[2] == 1)
                    ? world.input.game_key_alt_names[static_cast<std::size_t>(narrow[1])]
                    : world.input.game_key_names[static_cast<std::size_t>(narrow[1])];
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] = wse_utf8_to_utf16(name);
        }
        return NativeDispatch::handled;
    case 70: // key_is_down (0x1401c6965)
        return native_world_check_key_state(
            narrow[0], statement.numOperands >= 2 && narrow[1] != 0,
            world.input.key_down, world);
    case 71: // key_clicked (0x1401c69bf)
        return native_world_check_key_state(
            narrow[0], statement.numOperands >= 2 && narrow[1] != 0,
            world.input.key_clicked, world);
    case 72: // game_key_is_down (0x1401c6a19)
        return native_world_check_key_state(
            narrow[0], statement.numOperands >= 2 && narrow[1] != 0,
            world.input.game_key_down, world);
    case 73: // game_key_clicked (0x1401c6a71)
        return native_world_check_key_state(
            narrow[0], statement.numOperands >= 2 && narrow[1] != 0,
            world.input.game_key_clicked, world);
    case 77: // omit_key_once (0x1401c6b35)
        if (narrow[0] >= 0 && narrow[0] < game::kMaxRawKeys) {
            world.input.omitted_keys[static_cast<std::size_t>(narrow[0])] = true;
        }
        return NativeDispatch::handled;

    // -- positions_and_math (4): 707, 750, 1628, 1629 --
    case 707: { // position_has_line_of_sight_to_position (0x1401cadcc)
        if (!world.lighting.has_scene || !wse_register_ok(narrow[0]) ||
            !wse_register_ok(narrow[1])) {
            return NativeDispatch::condition_false;
        }
        const auto& a = ctx.positions[static_cast<std::size_t>(narrow[0])].o;
        const auto& b = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
        wse::core::WseVec4 diff{b.x - a.x, b.y - a.y, b.z - a.z, 0.0f};
        const float len = diff.normalize_with_length();
        const float seg_len = std::min(len, 100000.0f);
        if (seg_len <= 1e-6f) {
            return NativeDispatch::handled;
        }
        const NativeRayHitResult hit =
            native_world_ray_aabb_hit(world, a, diff, seg_len, 0xffffffffu);
        return hit.hit ? NativeDispatch::condition_false : NativeDispatch::handled;
    }
    case 750: { // position_get_screen_projection (0x1401cbdbc -> rglCamera 0x1403005c0)
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1])) {
            const auto& p = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            const auto& cam = world.misc.camera_transform;
            const float vx = p.x - cam.o.x;
            const float vy = p.y - cam.o.y;
            const float vz = p.z - cam.o.z;
            // Warband rglTransform camera frame: s = right, f = up, -u = forward.
            const float cx = vx * cam.s.x + vy * cam.s.y + vz * cam.s.z;
            const float cy = vx * cam.f.x + vy * cam.f.y + vz * cam.f.z;
            const float cz = -(vx * cam.u.x + vy * cam.u.y + vz * cam.u.z);
            auto& out = ctx.positions[static_cast<std::size_t>(narrow[0])].o;
            if (cz <= 1e-4f) {
                out = wse::core::WseVec4{-1.0f, -1.0f, 0.0f, 0.0f};
            } else {
                const game::CameraProjectionScales proj = game::camera_projection_scales(
                    world.misc.camera_fov_deg, world.misc.viewport_width,
                    world.misc.viewport_height);
                const float ndc_x = cx / (cz * proj.tan_half_fov * proj.aspect);
                const float ndc_y = cy / (cz * proj.tan_half_fov);
                out = wse::core::WseVec4{(ndc_x + 1.0f) * 0.5f,
                                         (ndc_y + 1.0f) * 0.5f * 0.75f, 0.0f, 0.0f};
            }
        }
        return NativeDispatch::handled;
    }
    case 1628: // map_get_land_position_around_position (0x1401d20f9)
    case 1629: { // map_get_water_position_around_position (0x1401d2181)
        if (wse_register_ok(narrow[0]) && wse_register_ok(narrow[1]) && wide[2] >= 0) {
            const float radius = static_cast<float>(wide[2]) / fp;
            const auto& src = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            float dx = 0.0f;
            float dy = 0.0f;
            if (radius > 0.0f) {
                const float angle = native_entity_randf(ctx) * 6.2831853f;
                const float dist = std::sqrt(native_entity_randf(ctx)) * radius;
                dx = std::cos(angle) * dist;
                dy = std::sin(angle) * dist;
            }
            ctx.positions[static_cast<std::size_t>(narrow[0])].o.x = src.x + dx;
            ctx.positions[static_cast<std::size_t>(narrow[0])].o.y = src.y + dy;
        }
        return NativeDispatch::handled;
    }

    // -- strings (7): 2337, 2340, 2345, 2351, 2352, 2353, 2747 --
    case 2337: // str_store_info_page_name (0x1401dee99)
        if (wse_register_ok(narrow[0]) && world.is_valid_info_page(narrow[1])) {
            const auto banks = native_string_banks(ctx);
            const std::u16string raw = wse_utf8_to_utf16(
                world.info_pages[static_cast<std::size_t>(narrow[1])].title);
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_parse_string(raw, ctx.registers, banks);
        }
        return NativeDispatch::handled;
    case 2340: // str_store_date (0x1401def1f)
        if (wse_register_ok(narrow[0]) && narrow[1] >= 0) {
            world.result_string.clear();
            if (ctx.scripts != nullptr) {
                const std::int64_t params[2] = {
                    static_cast<std::int64_t>(narrow[1] / 24),
                    static_cast<std::int64_t>(narrow[1])};
                (void)execute_mapped_script(*ctx.scripts, 19U, ctx, params);
            }
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(world.result_string);
        }
        return NativeDispatch::handled;
    case 2345: // str_store_info_page_name_link (0x1401df1d4)
        if (wse_register_ok(narrow[0]) && world.is_valid_info_page(narrow[1])) {
            native_store_name_link(narrow[0], "ip", narrow[1],
                                   world.info_pages[static_cast<std::size_t>(narrow[1])].title,
                                   ctx);
        }
        return NativeDispatch::handled;
    case 2351: // str_store_server_password (0x1401df2f5)
        if (wse_register_ok(narrow[0])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(world.mp.server_password);
        }
        return NativeDispatch::handled;
    case 2352: // str_store_server_name (0x1401df311)
        if (wse_register_ok(narrow[0])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(world.mp.server_name);
        }
        return NativeDispatch::handled;
    case 2353: // str_store_welcome_message (0x1401df32d)
        if (wse_register_ok(narrow[0])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(world.mp.welcome_message);
        }
        return NativeDispatch::handled;
    case 2747: // str_store_player_face_keys (0x1401e13d8)
        if (wse_register_ok(narrow[0]) && world.is_valid_player(narrow[1])) {
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] = wse_utf8_to_utf16(
                world.players[static_cast<std::size_t>(narrow[1])].face_keys.to_string());
        }
        return NativeDispatch::handled;

    // -- factions_quests_menus_options (4): 1098, 1275, 1276, 1277 --
    case 1098: // quest_set_note_available (0x1401cda75)
        if (world.is_valid_quest(narrow[0])) {
            world.quests[static_cast<std::size_t>(narrow[0])].note_available = (wide[1] != 0);
        }
        return NativeDispatch::handled;
    case 1275: // faction_set_name (0x1401cfd05)
        if (world.is_valid_faction(narrow[0]) &&
            wse_string_register_ok(narrow[1], tag1, native_string_banks(ctx))) {
            world.factions[static_cast<std::size_t>(narrow[0])].name =
                wse_utf16_to_utf8(native_resolve_and_parse(narrow[1], tag1, ctx));
        }
        return NativeDispatch::handled;
    case 1276: // faction_set_color (0x1401cfd90)
        if (world.is_valid_faction(narrow[0])) {
            world.factions[static_cast<std::size_t>(narrow[0])].color =
                static_cast<std::uint32_t>(narrow[1]);
        }
        return NativeDispatch::handled;
    case 1277: // faction_get_color (0x1401cfdc1)
        if (world.is_valid_faction(narrow[1])) {
            store_lvalue(dst,
                         static_cast<std::int64_t>(
                             world.factions[static_cast<std::size_t>(narrow[1])].color),
                         ctx, locals);
        }
        return NativeDispatch::handled;

    // -- items (5): 1964, 1978, 1981, 2725, 2726 --
    case 1964: { // cur_item_add_mesh (0x1401d9b19)
        if (ctx.currentCaller != -50 || world.cur_item_meta_mesh == nullptr ||
            !wse_string_register_ok(narrow[0], tag0, native_string_banks(ctx))) {
            return NativeDispatch::handled;
        }
        std::string mesh_name = wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx));
        native_world_spaces_to_underscores(mesh_name);
        const auto it = world.resource_meshes.find(mesh_name);
        if (it == world.resource_meshes.end()) {
            return NativeDispatch::handled;
        }
        auto& meta = *world.cur_item_meta_mesh;
        const auto& src_meshes = it->second;
        const std::int32_t lod_begin = narrow[1];
        std::int32_t lod_end = narrow[2];
        if (lod_end == 0) {
            lod_end = static_cast<std::int32_t>(meta.lods.size());
        }
        for (std::int32_t lod = std::max<std::int32_t>(0, lod_begin);
             lod < lod_end && static_cast<std::size_t>(lod) < src_meshes.size(); ++lod) {
            const auto u_lod = static_cast<std::size_t>(lod);
            if (u_lod >= game::WseMetaMesh::kMaxLods) {
                break;
            }
            game::WseMesh clone = src_meshes[u_lod];
            if (narrow[3] > 0) {
                const std::uint32_t a_bits =
                    static_cast<std::uint32_t>(narrow[3] & 0xff) << 24U;
                clone.material_color = (clone.material_color & 0x00ffffffu) | a_bits;
                if (clone.has_vertex_color_override) {
                    clone.vertex_color_override =
                        (clone.vertex_color_override & 0x00ffffffu) | a_bits;
                }
                for (std::uint32_t& vc : clone.vertex_colors) {
                    vc = (vc & 0x00ffffffu) | a_bits;
                }
            }
            if (meta.lods.size() <= u_lod) {
                meta.lods.resize(u_lod + 1U);
            }
            meta.lods[u_lod].push_back(std::move(clone));
        }
        return NativeDispatch::handled;
    }
    case 1978: { // cur_item_set_material (0x1401da2fc)
        if (ctx.currentCaller != -50 || world.cur_item_meta_mesh == nullptr ||
            !wse_string_register_ok(narrow[0], tag0, native_string_banks(ctx))) {
            return NativeDispatch::handled;
        }
        std::string mat_name = wse_utf16_to_utf8(native_resolve_and_parse(narrow[0], tag0, ctx));
        native_world_spaces_to_underscores(mat_name);
        const auto mat_it = world.resource_materials.find(mat_name);
        const std::string resolved_mat =
            (mat_it != world.resource_materials.end() && mat_it->second) ? mat_name : std::string{};
        auto& meta = *world.cur_item_meta_mesh;
        const std::int32_t mesh_idx = narrow[1];
        const std::int32_t lod_begin = std::max<std::int32_t>(0, narrow[2]);
        std::int32_t lod_end =
            (narrow[3] == 0) ? static_cast<std::int32_t>(meta.lods.size()) : narrow[3];
        lod_end = std::min(lod_end, static_cast<std::int32_t>(meta.lods.size()));
        for (std::int32_t lod = lod_begin; lod < lod_end; ++lod) {
            auto& meshes = meta.lods[static_cast<std::size_t>(lod)];
            if (mesh_idx < 0) {
                for (auto& mesh : meshes) {
                    mesh.set_material(resolved_mat);
                }
            } else if (static_cast<std::size_t>(mesh_idx) < meshes.size()) {
                meshes[static_cast<std::size_t>(mesh_idx)].set_material(resolved_mat);
            }
        }
        return NativeDispatch::handled;
    }
    case 1981: { // cur_item_set_tableau_material (0x1401da509)
        if (ctx.currentCaller == -50 && world.cur_item_meta_mesh != nullptr &&
            world.is_valid_tableau_material(narrow[0])) {
            world.fx.cur_item_tableau_material_no = narrow[0];
            world.fx.cur_item_tableau_param = narrow[1];
            const std::string tab_mat = "tableau_" + std::to_string(narrow[0]);
            for (auto& meshes : world.cur_item_meta_mesh->lods) {
                for (auto& mesh : meshes) {
                    mesh.set_material(tab_mat);
                }
            }
        }
        return NativeDispatch::handled;
    }
    case 2725: // item_has_modifier (0x1401e1357)
        if (!world.is_valid_item(narrow[0])) {
            return NativeDispatch::condition_false;
        }
        return ((world.items[static_cast<std::size_t>(narrow[0])].modifiers >>
                 (static_cast<std::uint32_t>(narrow[1]) & 0x3fU)) &
                1ULL) != 0ULL
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    case 2726: { // item_has_faction (0x1401e1396)
        if (!world.is_valid_item(narrow[0]) || !world.is_valid_faction(narrow[1])) {
            return NativeDispatch::condition_false;
        }
        const auto& factions = world.items[static_cast<std::size_t>(narrow[0])].factions;
        if (factions.empty()) {
            return NativeDispatch::handled;
        }
        return std::find(factions.begin(), factions.end(), narrow[1]) != factions.end()
                   ? NativeDispatch::handled
                   : NativeDispatch::condition_false;
    }

    // -- parties (14): 110, 1110, 1116, 1119, 1320, 1663, 1666, 1667, 1678, 1679, 1682, 1697, 1698, 2050 --
    case 110: { // main_party_has_troop (0x1401c6e51)
        if (!world.is_valid_party(world.main_party_no)) {
            return NativeDispatch::condition_false;
        }
        const auto& stacks = world.parties[static_cast<std::size_t>(world.main_party_no)].stacks;
        for (const auto& st : stacks) {
            if (st.troop_no == narrow[0]) {
                return NativeDispatch::handled;
            }
        }
        return NativeDispatch::condition_false;
    }
    case 1110: // add_party_note_tableau_mesh (0x1401cddfe)
        if (world.is_valid_party(narrow[0]) && world.is_valid_tableau_material(narrow[1])) {
            world.parties[static_cast<std::size_t>(narrow[0])].note_tableau_material_no =
                narrow[1];
        }
        return NativeDispatch::handled;
    case 1116: // add_party_note_from_dialog (0x1401ce217)
        if (ctx.currentCaller == 4 && world.is_valid_party(narrow[0])) {
            native_world_assign_dialog_note(
                world.parties[static_cast<std::size_t>(narrow[0])].notes, narrow[1],
                narrow[2] != 0, world);
        }
        return NativeDispatch::handled;
    case 1119: // add_party_note_from_sreg (0x1401ce49d)
        if (world.is_valid_party(narrow[0])) {
            native_world_assign_sreg_note(
                world.parties[static_cast<std::size_t>(narrow[0])].notes, narrow[1], narrow[2],
                tag2, narrow[3] != 0, ctx);
        }
        return NativeDispatch::handled;
    case 1320: // set_mercenary_source_party (0x1401d010d)
        if (world.is_valid_party(narrow[0])) {
            world.misc.mercenary_source_party_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 1663: // party_quick_attach_to_current_battle (0x1401d2c50)
        game::party_attach_to_current_battle(world, narrow[0], narrow[1]);
        return NativeDispatch::handled;
    case 1666: // party_leave_cur_battle (0x1401d2ce8)
        game::party_leave_cur_battle(world, narrow[0]);
        return NativeDispatch::handled;
    case 1667: // party_set_next_battle_simulation_time (0x1401d2d15)
        if (world.is_valid_party(narrow[0])) {
            const auto& p = world.parties[static_cast<std::size_t>(narrow[0])];
            if (p.battle_event_index >= 0 &&
                static_cast<std::size_t>(p.battle_event_index) < world.battle_events.size()) {
                world.battle_events[static_cast<std::size_t>(p.battle_event_index)]
                    .simulation_time = static_cast<float>(wide[1]);
            } else if (p.in_battle) {
                world.misc.battle_simulation_time = static_cast<float>(wide[1]);
            }
        }
        return NativeDispatch::handled;
    case 1678: // party_add_particle_system (0x1401d31ef)
        if (world.is_valid_party(narrow[0]) && world.is_valid_particle_system(narrow[1])) {
            game::push_bounded(
                world.parties[static_cast<std::size_t>(narrow[0])].particle_system_nos,
                narrow[1], game::kMaxPartyParticleSystems);
        }
        return NativeDispatch::handled;
    case 1679: // party_clear_particle_systems (0x1401d3231)
        if (world.is_valid_party(narrow[0])) {
            world.parties[static_cast<std::size_t>(narrow[0])].particle_system_nos.clear();
        }
        return NativeDispatch::handled;
    case 1682: // party_set_extra_icon (0x1401d33a1)
        if (world.is_valid_party(narrow[0]) && world.is_valid_map_icon(narrow[1]) &&
            wide[2] >= 0 && wide[3] >= 0 && wide[4] >= 0 && wide[5] >= 0) {
            auto& p = world.parties[static_cast<std::size_t>(narrow[0])];
            p.extra_icon_no = narrow[1];
            p.extra_icon_up_down_distance = static_cast<float>(wide[2]) / fp;
            p.extra_icon_up_down_frequency = static_cast<float>(wide[3]) / fp;
            p.extra_icon_rotate_frequency = static_cast<float>(wide[4]) / fp;
            p.extra_icon_fade_distance = static_cast<float>(wide[5]) / fp;
        }
        return NativeDispatch::handled;
    case 1697: { // inflict_casualties_to_party_group (0x1401d3783)
        if (world.is_valid_party(narrow[0])) {
            const std::int32_t cas_party =
                (narrow[2] == world.misc.camp_bandits_party_no ||
                 !world.is_valid_party(narrow[2]))
                    ? -1
                    : narrow[2];
            game::party_inflict_casualties_group(world, narrow[0], narrow[1], cas_party,
                                                 static_cast<std::uint32_t>(ctx.randSeed));
        }
        return NativeDispatch::handled;
    }
    case 1698: // distribute_party_among_party_group (0x1401d37f4)
        game::party_distribute_among_group(world, narrow[0], narrow[1]);
        return NativeDispatch::handled;
    case 2050: // change_screen_exchange_with_party (0x1401dc209)
        if (world.is_valid_party(narrow[0])) {
            world.ui.exchange_party_no = narrow[0];
            world.ui.exchange_window_open = true;
        }
        return NativeDispatch::handled;

    // -- troops (10): 1108, 1114, 1503, 1995, 2160, 2161, 2197, 2200, 2231, 2388 --
    case 1108: // add_troop_note_tableau_mesh (0x1401cdd7e)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_tableau_material(narrow[1])) {
            world.troops[static_cast<std::size_t>(narrow[0])].note_tableau_material_no =
                narrow[1];
        }
        return NativeDispatch::handled;
    case 1114: // add_troop_note_from_dialog (0x1401ce032)
        if (ctx.currentCaller == 4 && world.is_valid_troop(narrow[0])) {
            native_world_assign_dialog_note(
                world.troops[static_cast<std::size_t>(narrow[0])].notes, narrow[1],
                narrow[2] != 0, world);
        }
        return NativeDispatch::handled;
    case 1503: // troop_set_face_key_from_current_profile (0x1401d02ef)
        if (world.is_valid_troop(narrow[0]) && world.is_valid_profile(world.mp.current_profile_no)) {
            world.troops[static_cast<std::size_t>(narrow[0])].face_keys[0] =
                world.mp.profiles[static_cast<std::size_t>(world.mp.current_profile_no)]
                    .face_keys.keys;
        }
        return NativeDispatch::handled;
    case 1995: // cur_tableau_add_troop (0x1401dabc0)
        if (world.is_valid_troop(narrow[0]) && wse_register_ok(narrow[1]) &&
            world.is_valid_action(narrow[2]) && world.tableau.active) {
            game::WseTableauRenderItem item{};
            item.kind = game::WseTableauRenderItem::Kind::troop;
            item.id = narrow[0];
            item.position = ctx.positions[static_cast<std::size_t>(narrow[1])];
            item.param = narrow[2];
            item.override_horse_item_no = (narrow[3] > 0) ? narrow[3] : -1;
            game::push_bounded(world.tableau.items, std::move(item), game::kMaxTableauItems);
        }
        return NativeDispatch::handled;
    case 2160: // store_troop_count_companions (0x1401dd935)
        if (world.is_valid_party(narrow[2])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    game::party_count_troop_type(world, narrow[2], narrow[1], false)),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2161: // store_troop_count_prisoners (0x1401dd980)
        if (world.is_valid_party(narrow[2])) {
            store_lvalue(
                dst,
                static_cast<std::int64_t>(
                    game::party_count_troop_type(world, narrow[2], narrow[1], true)),
                ctx, locals);
        }
        return NativeDispatch::handled;
    case 2197: // set_conversation_speaker_troop (0x1401dddd6)
        if (world.is_valid_troop(narrow[0])) {
            world.dialog.speaker_troop_no = narrow[0];
        }
        return NativeDispatch::handled;
    case 2200: // store_conversation_troop (0x1401dde43)
        store_lvalue(dst, static_cast<std::int64_t>(world.conversation_partner_troop_no), ctx,
                     locals);
        return NativeDispatch::handled;
    case 2231: // store_troop_value (0x1401de05d)
        if (world.is_valid_troop(narrow[1])) {
            std::int32_t join_cost = 0;
            world.trigger_result = 0;
            bool handled_script = false;
            if (ctx.scripts != nullptr) {
                const std::int64_t params[2] = {
                    static_cast<std::int64_t>(narrow[1]),
                    static_cast<std::int64_t>(world.misc.camp_bandits_party_no)};
                handled_script = execute_mapped_script(*ctx.scripts, 13U, ctx, params);
            }
            if (handled_script) {
                join_cost = static_cast<std::int32_t>(world.trigger_result);
            } else {
                const auto& tr = world.troops[static_cast<std::size_t>(narrow[1])];
                const std::int32_t level = tr.level;
                const std::int32_t wage = std::max<std::int32_t>(1, level + 1);
                join_cost = static_cast<std::int32_t>(static_cast<float>(wage) * 5.0f + 2.0f);
            }
            store_lvalue(dst, static_cast<std::int64_t>(join_cost), ctx, locals);
        }
        return NativeDispatch::handled;
    case 2388: // show_troop_details (0x1401df900)
        if (world.is_valid_troop(narrow[0]) && wse_register_ok(narrow[1]) && narrow[2] >= 0) {
            const auto& pos = ctx.positions[static_cast<std::size_t>(narrow[1])].o;
            world.ui.troop_tooltip.visible = true;
            world.ui.troop_tooltip.troop_no = narrow[0];
            world.ui.troop_tooltip.screen_pos = wse::core::WseVec2{pos.x, pos.y};
            world.ui.troop_tooltip.mode = narrow[2];
        }
        return NativeDispatch::handled;

    // -- face_keys (16): 2752..2767 --
    case 2752: // face_keys_get_hair (0x1401e17a1)
        native_world_fk_get6(dst, narrow[1], tag1, 0U, ctx, locals);
        return NativeDispatch::handled;
    case 2753: // face_keys_set_hair (0x1401e1849)
        native_world_fk_set6(narrow[0], tag0, narrow[1], 0U, ctx);
        return NativeDispatch::handled;
    case 2754: // face_keys_get_beard (0x1401e1914)
        native_world_fk_get6(dst, narrow[1], tag1, 6U, ctx, locals);
        return NativeDispatch::handled;
    case 2755: // face_keys_set_beard (0x1401e19bc)
        native_world_fk_set6(narrow[0], tag0, narrow[1], 6U, ctx);
        return NativeDispatch::handled;
    case 2756: // face_keys_get_face_texture (0x1401e1a87)
        native_world_fk_get6(dst, narrow[1], tag1, 12U, ctx, locals);
        return NativeDispatch::handled;
    case 2757: // face_keys_set_face_texture (0x1401e1b2e)
        native_world_fk_set6(narrow[0], tag0, narrow[1], 12U, ctx);
        return NativeDispatch::handled;
    case 2758: // face_keys_get_hair_texture (0x1401e1bf7)
        native_world_fk_get6(dst, narrow[1], tag1, 18U, ctx, locals);
        return NativeDispatch::handled;
    case 2759: // face_keys_set_hair_texture (0x1401e1c9e)
        native_world_fk_set6(narrow[0], tag0, narrow[1], 18U, ctx);
        return NativeDispatch::handled;
    case 2760: // face_keys_get_hair_color (0x1401e1d67)
        native_world_fk_get6(dst, narrow[1], tag1, 24U, ctx, locals);
        return NativeDispatch::handled;
    case 2761: // face_keys_set_hair_color (0x1401e1e1a)
        native_world_fk_set6(narrow[0], tag0, narrow[1], 24U, ctx);
        return NativeDispatch::handled;
    case 2762: // face_keys_get_age (0x1401e1ef0)
        native_world_fk_get6(dst, narrow[1], tag1, 30U, ctx, locals);
        return NativeDispatch::handled;
    case 2763: // face_keys_set_age (0x1401e1fa3)
        native_world_fk_set6(narrow[0], tag0, narrow[1], 30U, ctx);
        return NativeDispatch::handled;
    case 2764: // face_keys_get_skin_color (0x1401e2079)
        native_world_fk_get6(dst, narrow[1], tag1, 36U, ctx, locals);
        return NativeDispatch::handled;
    case 2765: // face_keys_set_skin_color (0x1401e212c)
        native_world_fk_set6(narrow[0], tag0, narrow[1], 36U, ctx);
        return NativeDispatch::handled;
    case 2766: // face_keys_get_morph_key (0x1401e2202)
        if (wse_string_register_ok(narrow[1], tag1, native_string_banks(ctx)) &&
            narrow[2] >= 0 && narrow[2] < game::kNumFaceMorphKeys) {
            const game::WseFaceKeys fk = native_world_parse_face_keys(narrow[1], tag1, ctx);
            store_lvalue(dst, static_cast<std::int64_t>(fk.get_morph_key(narrow[2])), ctx, locals);
        }
        return NativeDispatch::handled;
    case 2767: // face_keys_set_morph_key (0x1401e22d0)
        if (wse_register_ok(narrow[0]) && narrow[1] >= 0 &&
            narrow[1] < game::kNumFaceMorphKeys) {
            game::WseFaceKeys fk = native_world_parse_face_keys(narrow[0], tag0, ctx);
            fk.set_morph_key(narrow[1], narrow[2]);
            ctx.stringRegisters[static_cast<std::size_t>(narrow[0])] =
                wse_utf8_to_utf16(fk.to_string());
        }
        return NativeDispatch::handled;

    default:
        break;
    }
    return dispatch_native_world_2_statement(opcode, wide, narrow, tags, statement, ctx, locals);
}

}  // namespace wse::vm

#include "wse/vm/native_world_ops_2.h"
