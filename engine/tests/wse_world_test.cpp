// Host tests for the portable troop, item, and edit-mode executeWSE handlers.
//
// Build/run: powershell -File engine/build_host.ps1

#include "wse_test_common.h"

namespace {

using namespace wse::test;

void test_troop_group() {
    WseEffect effect;
    wse::game::WseWorldState world{};
    world.skills[wse::game::kSkillIronflesh].max_level = 10;
    world.skills[5].max_level = 6;
    world.skills[7].max_level = 10;
    world.skills[8].max_level = 10;
    world.skills[41].max_level = 10;

    world.troops.push_back(wse::game::WseTroop{});
    auto& troop = world.troops[0];
    troop.flags = 0x14ULL;
    troop.attributes[0] = 10;
    troop.hit_points = 45.0f;

    WseVmState state{};
    state.world = &world;

    // 3700..3705 points getters/setters.
    expect(run(opcode(WseOpcode::troop_set_skill_points), {0, 3}, {0, 3},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill_points), {0, -1}, {0, -1},
                   effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::troop_get_skill_points), {0, 0}, {0, 0},
                   effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 3 &&
               run(opcode(WseOpcode::troop_set_attribute_points), {0, 5},
                   {0, 5}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_get_attribute_points), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 5 &&
               run(opcode(WseOpcode::troop_set_proficiency_points), {0, 27},
                   {0, 27}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_get_proficiency_points), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 27,
           "troop points getters/setters preserve values on negative input");

    // 3706 troop_has_flag.
    expect(run(opcode(WseOpcode::troop_has_flag), {0, 0x04LL}, {0, 4}, effect,
               {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_has_flag), {0, 0x02LL}, {0, 2},
                   effect, {}, {}, state) == WseDispatch::condition_false &&
               run(opcode(WseOpcode::troop_has_flag), {9, 0x04LL}, {9, 4},
                   effect, {}, {}, state) == WseDispatch::ignored,
           "troop_has_flag tests flag bits and ignores invalid troop");

    // 3707 troop_set_skill (nibble packing, max_level clamp, Ironflesh HP + cap).
    expect(run(opcode(WseOpcode::troop_set_skill), {0, 5, 99}, {0, 5, 99},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill), {0, 7, 3}, {0, 7, 3},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill), {0, 8, 4}, {0, 8, 4},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill), {0, 41, 5}, {0, 41, 5},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               troop.get_skill_raw(5) == 6 && troop.get_skill_raw(7) == 3 &&
               troop.get_skill_raw(8) == 4 && troop.get_skill_raw(41) == 5,
           "troop_set_skill packs 4-bit nibbles without clobbering neighbors");

    expect(run(opcode(WseOpcode::troop_set_skill),
               {0, wse::game::kSkillIronflesh, 4},
               {0, wse::game::kSkillIronflesh, 4}, effect, {}, {}, state) ==
               WseDispatch::handled_no_store &&
               close(troop.hit_points, 53.0f),
           "troop_set_skill adds 2 HP per gained Ironflesh level");

    troop.hit_points = 900.0f;
    expect(run(opcode(WseOpcode::troop_set_skill),
               {0, wse::game::kSkillIronflesh, 2},
               {0, wse::game::kSkillIronflesh, 2}, effect, {}, {}, state) ==
               WseDispatch::handled_no_store &&
               close(troop.hit_points, 49.0f),
           "troop_set_skill caps HP to str + ironflesh*2 + 35");

    // 3708 troop_set_attribute & 3709 troop_set_proficiency.
    expect(run(opcode(WseOpcode::troop_set_attribute), {0, 0, 2}, {0, 0, 2},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               troop.attributes[0] == 2 && close(troop.hit_points, 41.0f) &&
               run(opcode(WseOpcode::troop_set_attribute), {0, 1, -5},
                   {0, 1, -5}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               troop.attributes[1] == 0 &&
               run(opcode(WseOpcode::troop_set_proficiency), {0, 0, 1200},
                   {0, 0, 1200}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               close(troop.proficiencies[0], 999.0f) &&
               run(opcode(WseOpcode::troop_set_proficiency), {0, 1, -10},
                   {0, 1, -10}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               close(troop.proficiencies[1], 0.0f) &&
               run(opcode(WseOpcode::troop_set_proficiency), {0, 7, 100},
                   {0, 7, 100}, effect, {}, {}, state) == WseDispatch::ignored,
           "troop_set_attribute caps HP and troop_set_proficiency clamps to [0, 999]");
}

void test_item_group() {
    WseEffect effect;
    std::array<std::u16string, 128> registers{};
    registers[1] = u"iron plate";
    registers[2] = u"heavy {s1} mesh";
    registers[3] = u"metal armor";
    registers[4] = u"horse_skel";

    wse::game::WseWorldState world{};
    world.num_particle_systems = 10;
    world.items.push_back(wse::game::WseItemKind{});
    world.items[0].id = "itm_war_sword";
    world.items[0].variations = {{"war_sword_mesh", 0}};
    world.items[0].thrust_damage = (2 << 8) | 18;
    world.items[0].swing_damage = (0 << 8) | 30;

    wse::game::WseMetaMesh cur_meta{};
    wse::game::WseMesh lod0_mesh{};
    lod0_mesh.vertex_colors = {0xffffffffu, 0xffffffffu};
    cur_meta.lods = {{lod0_mesh}, {lod0_mesh, lod0_mesh}};
    world.cur_item_meta_mesh = &cur_meta;

    wse::game::WseMesh tmpl0{};
    tmpl0.name = "heavy0";
    tmpl0.vertex_colors = {0x10101010u};
    wse::game::WseMesh tmpl1{};
    tmpl1.name = "heavy1";
    tmpl1.vertex_colors = {0x20202020u};
    wse::game::WseMesh tmpl2{};
    tmpl2.name = "heavy2";
    tmpl2.vertex_colors = {0x30303030u};
    world.resource_meshes["heavy_iron_plate_mesh"] = {tmpl0, tmpl1, tmpl2};
    world.resource_materials["metal_armor"] = true;
    world.resource_skeleton_models["horse_skel"] = true;

    WseVmState state{};
    state.world = &world;
    state.strings = {registers, {}, {}};

    // 3800..3810 item setters.
    expect(run(opcode(WseOpcode::item_set_thrust_damage), {0, 300}, {0, 300},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.items[0].thrust_damage == ((2 << 8) | 44) &&
               run(opcode(WseOpcode::item_set_thrust_damage_type), {0, 5},
                   {0, 5}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.items[0].thrust_damage == ((1 << 8) | 44) &&
               run(opcode(WseOpcode::item_set_swing_damage), {0, 42}, {0, 42},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_swing_damage_type), {0, 2},
                   {0, 2}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.items[0].swing_damage == ((2 << 8) | 42),
           "item damage setters mask low byte and bits 8..9 independently");

    expect(run(opcode(WseOpcode::item_set_head_armor), {0, 11}, {0, 11},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_body_armor), {0, 22}, {0, 22},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_leg_armor), {0, 33}, {0, 33},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_speed_rating), {0, 95}, {0, 95},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_missile_speed), {0, 75}, {0, 75},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.items[0].head_armor == 11 &&
               world.items[0].body_armor == 22 &&
               world.items[0].leg_armor == 33 &&
               world.items[0].speed_rating == 95 &&
               world.items[0].missile_speed == 75,
           "item armor and speed setters update fields");

    expect(run(opcode(WseOpcode::item_set_horse_blood_particles), {0, 2, 10},
               {0, 2, 10}, effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::item_set_horse_blood_particles), {0, 2, 9},
                   {0, 2, 9}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.items[0].blood_particle_system_nos[0] == 2 &&
               world.items[0].blood_particle_system_nos[1] == 9 &&
               run(opcode(WseOpcode::item_set_horse_blood_color),
                   {0, static_cast<std::int32_t>(0xffaabbccu)},
                   {0, static_cast<std::int32_t>(0xffaabbccu)}, effect, {}, {},
                   state) == WseDispatch::handled_no_store &&
               world.items[0].blood_color == 0xffaabbccu,
           "item_set_horse_blood_{particles,color} validate particle systems and store color");

    // 3811 cur_item_mesh_set_color & 3812 cur_item_add_mesh_with_material (caller == -50).
    expect(run(opcode(WseOpcode::cur_item_mesh_set_color), {0, 0x00112233},
               {0, 0x00112233}, effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::cur_item_add_mesh_with_material), {2, 3},
                   {2, 3}, effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::ignored,
           "cur_item_* reject calls outside caller == -50");

    state.caller = -50;
    expect(run(opcode(WseOpcode::cur_item_mesh_set_color), {1, 0x00445566},
               {1, 0x00445566}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               cur_meta.lods[0][0].vertex_colors[0] == 0xffffffffu &&
               cur_meta.lods[1][1].vertex_colors[0] == 0x00445566u &&
               cur_meta.lods[1][1].vertex_colors[1] == 0x00445566u,
           "cur_item_mesh_set_color colors matching mesh index across active LODs");

    expect(run(opcode(WseOpcode::cur_item_add_mesh_with_material), {2, 3},
               {2, 3}, effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               cur_meta.lods.size() == 2 && cur_meta.lods[0].size() == 2 &&
               cur_meta.lods[1].size() == 3 &&
               cur_meta.lods[0][1].material_name == "metal_armor" &&
               !cur_meta.lods[0][1].has_vertex_color_override &&
               run(opcode(WseOpcode::cur_item_add_mesh_with_material),
                   {2, 3, 2, 3, 0x00778899}, {2, 3, 2, 3, 0x00778899}, effect,
                   {}, {}, state, {0, 0}) == WseDispatch::handled_no_store &&
               cur_meta.lods.size() == 3 &&
               cur_meta.lods[2][0].material_name == "metal_armor" &&
               cur_meta.lods[2][0].has_vertex_color_override &&
               cur_meta.lods[2][0].vertex_color_override == 0x00778899u,
           "cur_item_add_mesh_with_material parses {sN}, normalizes spaces, and clones LODs");

    // 3813 item_set_horse_skeleton_model & 4235/4239 item string getters.
    registers[5] = u"missing_skel";
    expect(run(opcode(WseOpcode::item_set_horse_skeleton_model), {0, 5}, {0, 5},
               effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               world.items[0].skeleton_model.empty() &&
               run(opcode(WseOpcode::item_set_horse_skeleton_model), {0, 4},
                   {0, 4}, effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               world.items[0].skeleton_model == "horse_skel" &&
               run(opcode(WseOpcode::str_store_item_id), {0, 0}, {0, 0}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"itm_war_sword" &&
               run(opcode(WseOpcode::str_store_item_mesh_name), {0, 0}, {0, 0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"war_sword_mesh",
           "item_set_horse_skeleton_model and str_store_item_{id,mesh_name} work cleanly");
}

void test_edit_mode_group() {
    WseEffect effect;
    wse::vm::WseOptionsState options{};
    wse::game::WseWorldState world{};
    world.mission_objects.resize(3);
    world.mission_objects[0].active = true;
    world.mission_objects[1].active = true;
    world.mission_objects[2].active = false;
    world.edit_mode.active = false;
    world.edit_mode.mode = 3;
    world.edit_mode.dialog_open = true;

    WseVmState state{};
    state.options = &options;
    state.world = &world;

    // Inactive edit mode queries.
    expect(run(opcode(WseOpcode::edit_mode_get_num_selected_prop_instances),
               {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == -1 &&
               run(opcode(WseOpcode::edit_mode_get_mode), {0}, {0}, effect, {},
                   {}, state) == WseDispatch::handled &&
               effect.value == 3 &&
               run(opcode(WseOpcode::edit_mode_in_edit_objects_mode), {}, {},
                   effect, {}, {}, state) == WseDispatch::condition_false &&
               run(opcode(WseOpcode::edit_mode_window_open), {}, {}, effect, {},
                   {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::ignored,
           "edit_mode queries handle inactive state and expose get_mode/dialog_open");

    // 4606 option flag + active prop selection & highlighted index adjustment.
    world.edit_mode.active = true;
    world.edit_mode.mode = 0;
    expect(!options.edit_mode_enabled &&
               run(opcode(WseOpcode::edit_mode_set_enabled), {1}, {1}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               options.edit_mode_enabled &&
               run(opcode(WseOpcode::edit_mode_in_edit_objects_mode), {}, {},
                   effect, {}, {}, state) == WseDispatch::handled_no_store,
           "edit_mode_set_enabled and edit_mode_in_edit_objects_mode reflect state");

    expect(run(opcode(WseOpcode::edit_mode_select_prop_instance), {2}, {2},
               effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {1}, {1},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_get_num_selected_prop_instances),
                   {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 2 &&
               run(opcode(WseOpcode::edit_mode_get_selected_prop_instance),
                   {0, 1}, {0, 1}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
               effect.value == 1 &&
               run(opcode(WseOpcode::edit_mode_get_selected_prop_instance),
                   {0, 9}, {0, 9}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
               effect.value == -1,
           "edit_mode prop selection deduplicates active instances and bounds index queries");

    // Highlight prop 1 (index 1), then deselect earlier prop 0 -> index decrements to 0.
    expect(run(opcode(WseOpcode::edit_mode_set_highlighted_prop_instance), {1},
               {1}, effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_get_highlighted_prop_instance),
                   {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 1 &&
               run(opcode(WseOpcode::edit_mode_deselect_prop_instance), {0},
                   {0}, effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.edit_mode.highlighted_index == 0 &&
               run(opcode(WseOpcode::edit_mode_get_highlighted_prop_instance),
                   {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 1 &&
               run(opcode(WseOpcode::edit_mode_deselect_prop_instance), {1},
                   {1}, effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.edit_mode.highlighted_index == -1 &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_set_highlighted_prop_instance),
                   {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_set_highlighted_prop_instance),
                   {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.edit_mode.highlighted_index == -1,
           "edit_mode deselect shifts highlighted_index and omitted set_highlighted clears it");
}

void test_agent_action_speed_group() {
    WseEffect effect;
    wse::game::WseWorldState world{};
    world.has_game = false;
    world.agents.push_back(wse::game::WseAgent{});

    WseVmState state{};
    state.world = &world;

    expect(run(opcode(WseOpcode::agent_set_action_speed_modifier), {0, 150},
               {0, 150}, effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::agent_get_action_speed_modifier), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::ignored,
           "agent_*_action_speed_modifier ignore when has_game is false");

    world.has_game = true;
    expect(run(opcode(WseOpcode::agent_set_action_speed_modifier), {0, -1},
               {0, -1}, effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::agent_set_action_speed_modifier), {0, 1001},
                   {0, 1001}, effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::agent_set_action_speed_modifier), {0, 150},
                   {0, 150}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               close(world.agents[0].action_speed_modifier, 1.5f) &&
               run(opcode(WseOpcode::agent_get_action_speed_modifier), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.store_value && effect.value == 150 &&
               run(opcode(WseOpcode::agent_set_action_speed_modifier), {0, 1000},
                   {0, 1000}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_action_speed_modifier), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 1000,
           "agent_*_action_speed_modifier validate [0,1000] and round *100");
}

void test_party_group() {
    WseEffect effect;
    wse::game::WseWorldState world{};
    world.troops.resize(3);
    world.troops[0].upgrade_troop_nos = {1, 2};

    wse::game::WseParty party{};
    party.id = "p_main";
    party.flags = (1ULL << 35) | 0x4ULL;
    party.banner_icon_no = 17;
    party.extra_icon_no = 23;
    party.player_no = 2;
    party.controller = 0;  // 0 == player, 1 == bot
    party.terrain_variant = 3;

    wse::game::WsePartyStack s0{0, 4, 2, 0};
    s0.xp = 124.6f;
    s0.num_upgradeable = 3;
    wse::game::WsePartyStack s1{1, 2, 1, 0};
    s1.xp = 10.2f;
    s1.num_upgradeable = 0;
    party.stacks = {s0, s1};
    world.parties.push_back(party);

    WseVmState state{};
    state.world = &world;

    expect(run(opcode(WseOpcode::party_stack_get_experience), {0, 0, 0},
               {0, 0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.store_value && effect.value == 125 &&
               run(opcode(WseOpcode::party_stack_get_num_upgradeable), {0, 0, 0},
                   {0, 0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 3 &&
               run(opcode(WseOpcode::party_stack_get_num_upgradeable), {0, 0, 9},
                   {0, 0, 9}, effect, {}, {}, state) == WseDispatch::ignored,
           "party_stack_get_{experience,num_upgradeable} read stack fields and check bounds");

    expect(run(opcode(WseOpcode::party_has_flag),
               {0, static_cast<std::int64_t>(1ULL << 35)}, {0, 0}, effect, {},
               {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::party_has_flag), {0, 8}, {0, 8}, effect,
                   {}, {}, state) == WseDispatch::condition_false,
           "party_has_flag tests full 64-bit mask");

    expect(run(opcode(WseOpcode::party_heal_members), {0, 0, 1}, {0, 0, 1},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.parties[0].stacks[0].num_wounded_troops == 1 &&
               run(opcode(WseOpcode::party_switch_stacks), {0, 0, 1}, {0, 0, 1},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.parties[0].stacks[0].troop_no == 1 &&
               world.parties[0].stacks[1].troop_no == 0,
           "party_heal_members heals partial count and party_switch_stacks swaps");

    expect(run(opcode(WseOpcode::party_stack_set_num_upgradeable), {0, 1, 5},
               {0, 1, 5}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.parties[0].stacks[1].num_upgradeable == 5 &&
               run(opcode(WseOpcode::party_stack_upgrade), {0, 1, 2, 0},
                   {0, 1, 2, 0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.parties[0].stacks[0].num_troops == 4 &&
               world.parties[0].stacks[1].num_troops == 2,
           "party_stack_upgrade merges partial upgraded troops into existing target stack");

    expect(run(opcode(WseOpcode::party_stack_upgrade), {0, 1, 10, 1},
               {0, 1, 10, 1}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.parties[0].stacks[1].troop_no == 2 &&
               world.parties[0].stacks[1].num_upgradeable == 0 &&
               run(opcode(WseOpcode::party_stack_upgrade), {0, 1, 1, 0},
                   {0, 1, 1, 0}, effect, {}, {}, state) ==
                   WseDispatch::ignored,
           "party_stack_upgrade converts stack in place when upgrading full stack and ignores invalid target");

    expect(run(opcode(WseOpcode::party_get_banner_icon), {0, 0}, {0, 0}, effect,
               {}, {}, state) == WseDispatch::handled &&
               effect.value == 17 &&
               run(opcode(WseOpcode::party_get_extra_icon), {0, 0}, {0, 0},
                   effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 23 &&
               run(opcode(WseOpcode::party_get_player_id), {0, 0}, {0, 0},
                   effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 2 &&
               run(opcode(WseOpcode::party_get_current_terrain_variant), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 3,
           "party_get_{banner_icon,extra_icon,player_id,current_terrain_variant} return fields");

    world.parties[0].player_no = -1;
    world.parties[0].controller = 1;
    expect(run(opcode(WseOpcode::party_is_non_player), {0}, {0}, effect, {}, {},
               state) == WseDispatch::handled_no_store,
           "party_is_non_player reflects controller/bot state");
    world.parties[0].controller = 0;
    expect(run(opcode(WseOpcode::party_is_non_player), {0}, {0}, effect, {}, {},
               state) == WseDispatch::condition_false,
           "party_is_non_player returns condition_false for player controller");
}

}  // namespace

namespace wse::selftest {

int run_wse_world_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_troop_group();
    test_item_group();
    test_edit_mode_group();
    test_agent_action_speed_group();
    test_party_group();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("wse_world tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_wse_world_suite(nullptr);
}
#endif
