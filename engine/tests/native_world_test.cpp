// Host unit tests for Milestone 4 extended native world/scene/UI/lighting/
// multiplayer handlers across native_world_ops{_1,_2,_3}.h, upgraded M3 hooks,
// and non-switch runtime opcodes.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "wse/vm/block_vm.h"

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

[[nodiscard]] bool close(float a, float b, float eps = 1e-4f) noexcept {
    return std::fabs(a - b) <= eps;
}

constexpr std::uint64_t tagged(std::uint8_t tag, std::uint64_t payload) noexcept {
    return (static_cast<std::uint64_t>(tag & 0x7FU) << 56) |
           (payload & wse::vm::kPayloadMask);
}

constexpr std::int64_t reg(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(
        tagged(1, static_cast<std::uint64_t>(idx)));
}

wse::vm::Statement make_stmt(std::uint32_t opcode,
                             std::initializer_list<std::int64_t> args) {
    wse::vm::Statement stmt{};
    stmt.opcode = opcode;
    stmt.numOperands = static_cast<std::int32_t>(args.size());
    std::size_t idx = 0;
    for (std::int64_t arg : args) {
        if (idx < wse::vm::kMaxOperands) {
            stmt.operands[idx++] = arg;
        }
    }
    return stmt;
}

bool run_stmt(wse::vm::VmContext& ctx,
              std::uint32_t opcode,
              std::initializer_list<std::int64_t> args,
              std::int32_t caller = 0) {
    std::vector<std::int64_t> locals(32, 0);
    const auto stmt = make_stmt(opcode, args);
    return wse::vm::execute_statement(stmt, caller, ctx, locals);
}

void init_world(wse::vm::VmContext& ctx) {
    auto& w = ctx.world;
    w.has_game = true;
    ctx.fixedPointMultiplier = 1000;
    w.troops.resize(4);
    w.troops[0].name = "Player";
    w.troops[0].flags = 0x10ULL;  // hero
    w.troops[0].hit_points = 35.0f;
    w.troops[1].name = "Speaker";
    w.troops[2].name = "Guard";
    w.troops[2].face_keys[0] = {0x1111ULL, 0x2222ULL, 0ULL, 0ULL};

    w.items.resize(4);
    w.parties.resize(5);
    w.main_party_no = 0;
    w.factions.resize(3);
    w.quests.resize(3);
    w.quests[0].id = "qst_test";
    w.quests[0].title = "Fallback {reg0} Title";
    w.quests[0].text = "Fallback^Body {s0}";
    w.sites.resize(3);
    w.mission_templates.resize(2);
    w.players.resize(5);
    w.players[0].name = "Host";
    w.players[0].unique_id = 1000;
    w.players[1].name = "RegularPlayer";
    w.players[1].unique_id = 2001;
    w.players[1].privilege = 0;
    w.players[2].name = "AdminPlayer";
    w.players[2].unique_id = 3001;
    w.players[2].privilege = 3;
    w.players[3].name = "BannerTarget";
    w.players[3].unique_id = 4001;
    w.agents.resize(4);
    w.agents[1].troop_no = 2;
    w.agents[1].dna = 7;
    w.agents[2].troop_no = 1;
    w.mission_objects.resize(3);
    w.mission_objects[0].is_item = true;
    w.mission_objects[0].scene_prop_no = 1;
    w.mission_objects[1].is_item = false;
    w.mission_objects[1].scene_prop_no = 1;
    w.info_pages.resize(2);
    w.mesh_names = {"mesh_zero", "mesh_bg"};
    w.misc.num_menus = 4;
    w.misc.num_tableau_materials = 6;
    w.misc.num_scene_props = 6;
    w.misc.num_post_effects = 4;
    w.num_particle_systems = 4;
    w.mp.profiles.resize(2);
    w.dialog.speaker_troop_no = 1;
    w.dialog.speaker_text = "Dialog note body";
}

void test_world_ops_1() {
    wse::vm::VmContext ctx;
    init_world(ctx);

    // 53 is_cheat_mode_enabled
    ctx.options.cheat_mode = false;
    expect(!run_stmt(ctx, 53U, {}), "53 false when cheat_mode off");
    ctx.options.cheat_mode = true;
    expect(run_stmt(ctx, 53U, {}), "53 true when cheat_mode on");

    // 2180 store_main_party_wounded: 3 wounded regulars + hero (max 35, hp 25 -> ceil(1.5)=2)
    ctx.world.troops[0].hit_points = 25.0f;
    wse::game::WsePartyStack hero_stack{};
    hero_stack.troop_no = 0;
    hero_stack.num_troops = 1;
    wse::game::WsePartyStack reg_stack{};
    reg_stack.troop_no = 2;
    reg_stack.num_troops = 10;
    reg_stack.num_wounded_troops = 3;
    wse::game::WsePartyStack pris_stack{};
    pris_stack.troop_no = 2;
    pris_stack.num_wounded_troops = 5;
    pris_stack.flags = 1U;
    ctx.world.parties[0].stacks = {hero_stack, reg_stack, pris_stack};
    expect(run_stmt(ctx, 2180U, {reg(0)}), "2180 store_main_party_wounded runs");
    expect(ctx.registers[0] == 5, "2180 counts 3 regular + 2 hero wounded, skips prisoners");

    // 2749 str_store_agent_face_keys
    ctx.world.agents[0].face_keys.keys = {0x12345678ULL, 0ULL, 0ULL, 0ULL};
    expect(run_stmt(ctx, 2749U, {0, 0}), "2749 stores explicit agent face_keys");
    expect(ctx.stringRegisters[0] == u"0000000012345678000000000000000000000000000000000000000000000000",
           "2749 explicit face_keys string");
    expect(run_stmt(ctx, 2749U, {1, 1}), "2749 falls back to troop face_keys + dna");
    expect(ctx.stringRegisters[1].size() == 64U &&
               ctx.stringRegisters[1].starts_with(u"0000000000001111"),
           "2749 copies upper key word and mixes lower");

    // 70 key_is_down & 71 key_clicked are not suppressed by 77 omit_key_once for scripts;
    // 77/78 still track omitted_keys for default handlers and game-key opcodes 72/73.
    ctx.world.input.key_down[65] = true;
    ctx.world.input.key_clicked[65] = true;
    expect(run_stmt(ctx, 70U, {65}) && run_stmt(ctx, 71U, {65}),
           "70/71 true before omit");
    expect(run_stmt(ctx, 77U, {65}), "77 omit_key_once");
    expect(ctx.world.input.omitted_keys[65] && run_stmt(ctx, 70U, {65}) &&
               run_stmt(ctx, 71U, {65}),
           "77 sets omitted_keys without suppressing raw script ops 70/71");
    expect(run_stmt(ctx, 78U, {}), "78 clear_omitted_keys");
    expect(!ctx.world.input.omitted_keys[65] && run_stmt(ctx, 70U, {65}),
           "78 clears omitted_keys");

    // 707 line of sight & 2050 & 2388
    ctx.positions[0].o = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx.positions[1].o = {10.0f, 0.0f, 0.0f, 1.0f};
    ctx.world.fx.ray_obstacles.push_back(
        {true, 0, 0xffffffffu, {4.0f, -1.0f, -1.0f}, {6.0f, 1.0f, 1.0f}});
    expect(!run_stmt(ctx, 707U, {0, 1}), "707 obstructed LOS returns false");
    ctx.world.fx.ray_obstacles.clear();
    expect(run_stmt(ctx, 707U, {0, 1}), "707 clear LOS returns true");
    expect(run_stmt(ctx, 2050U, {1}), "2050 exchange with party");
    expect(ctx.world.ui.exchange_window_open && ctx.world.ui.exchange_party_no == 1,
           "2050 sets exchange state");
    ctx.positions[2].o = {120.0f, 240.0f, 0.0f, 1.0f};
    expect(run_stmt(ctx, 2388U, {2, 2, 1}), "2388 show_troop_details");
    expect(ctx.world.ui.troop_tooltip.visible &&
               ctx.world.ui.troop_tooltip.troop_no == 2 &&
               close(ctx.world.ui.troop_tooltip.screen_pos.x, 120.0f),
           "2388 populates troop tooltip");
}

void test_world_ops_2_and_non_switch_and_m3_upgrades() {
    wse::vm::VmContext ctx;
    init_world(ctx);

    // _2 opcodes
    ctx.world.misc.repeat_object = 42;
    expect(run_stmt(ctx, 50U, {reg(0)}), "50 store_repeat_object");
    expect(ctx.registers[0] == 42, "50 value");
    expect(run_stmt(ctx, 55U, {reg(1)}), "55 get_operation_set_version");
    expect(ctx.registers[1] == 9999, "55 returns 9999");

    ctx.world.misc.trigger_params[0] = 77;
    ctx.world.misc.trigger_params[15] = 88;
    expect(run_stmt(ctx, 2070U, {reg(2), 16}), "2070 param 16");
    expect(ctx.registers[2] == 88, "2070 value");
    expect(run_stmt(ctx, 2070U, {reg(3), 17}), "2070 invalid id still handled");
    expect(ctx.errorCount == 1, "2070 invalid id reports error");
    expect(run_stmt(ctx, 2071U, {reg(4)}), "2071 param 1");
    expect(ctx.registers[4] == 77, "2071 value");

    ctx.world.tableau.active = true;
    expect(run_stmt(ctx, 1980U, {2, 9, 0, 0}), "1980 cur_tableau_add_tableau_mesh");
    expect(ctx.world.tableau.items.size() == 1U &&
               close(ctx.world.tableau.items[0].scale.x, 1.0f),
           "1980 pushes tableau item with default scale 1.0");

    // Non-switch runtime opcodes (473/477/1070/1104/1302/1491 true, 1004 false)
    for (std::uint32_t op : {473U, 477U, 1070U, 1104U, 1302U, 1491U}) {
        expect(run_stmt(ctx, op, {}), "non-switch no-op returns true");
    }
    expect(!run_stmt(ctx, 1004U, {}), "1004 returns condition false");
    expect(ctx.unimplementedCount == 0, "non-switch ops do not count unimplemented");

    // Upgraded M3 opcodes: 1802, 1930, 1969
    ctx.world.agents[0].has_relations = true;
    ctx.world.agents[0].relations = {{1, -1}};
    expect(run_stmt(ctx, 1802U, {0}), "1802 agent_clear_relations_with_agents");
    expect(!ctx.world.agents[0].has_relations && ctx.world.agents[0].relations.empty(),
           "1802 cleared relations");
    expect(run_stmt(ctx, 1930U, {2, 5}), "1930 add_reinforcements_to_entry");
    expect(ctx.world.fx.reinforcement_events.size() == 1U &&
               ctx.world.fx.reinforcement_events[0].entry_group_no == 2 &&
               ctx.world.fx.reinforcement_events[0].num_troops == 5,
           "1930 queued reinforcement event");
    expect(run_stmt(ctx, 1969U, {1, 0, 0}), "1969 particle_system_burst");
    expect(ctx.world.fx.particle_events.size() == 1U &&
               ctx.world.fx.particle_events[0].burst_strength == 100,
           "1969 defaulted strength to 100");
}

void test_ops3_options_profile_http_mod() {
    wse::vm::VmContext ctx;
    init_world(ctx);

    // 260..271 options
    expect(run_stmt(ctx, 261U, {0, 0}), "261 tier 0");
    expect(close(ctx.options.damage_to_player, 0.25f), "261 tier 0 -> 0.25");
    expect(run_stmt(ctx, 260U, {reg(0), 0}), "260 get tier");
    expect(ctx.registers[0] == 0, "260 tier 0");
    expect(run_stmt(ctx, 260U, {reg(1), 1}), "260 get pct");
    expect(ctx.registers[1] == 25, "260 25%");
    expect(run_stmt(ctx, 261U, {999, 1}), "261 out-of-range pct ignored");
    expect(close(ctx.options.damage_to_player, 0.25f), "261 unchanged");

    expect(run_stmt(ctx, 263U, {60, 1}), "263 pct 60");
    expect(close(ctx.options.damage_to_friends, 0.60f), "263 -> 0.60");
    expect(run_stmt(ctx, 262U, {reg(2), 0}), "262 tier");
    expect(ctx.registers[2] == 1, "262 0.60 -> tier 1");

    expect(run_stmt(ctx, 265U, {2}), "265 combat_ai=2");
    expect(run_stmt(ctx, 265U, {3}), "265 OOB ignored");
    expect(run_stmt(ctx, 264U, {reg(3)}), "264 get combat_ai");
    expect(ctx.registers[3] == 2, "264 == 2");
    expect(run_stmt(ctx, 267U, {0}), "267 campaign_ai=0");
    expect(run_stmt(ctx, 266U, {reg(4)}), "266 get campaign_ai");
    expect(ctx.registers[4] == 0, "266 == 0");
    expect(run_stmt(ctx, 269U, {4}), "269 combat_speed=4");
    expect(run_stmt(ctx, 269U, {5}), "269 OOB ignored");
    expect(run_stmt(ctx, 268U, {reg(5)}), "268 get combat_speed");
    expect(ctx.registers[5] == 4, "268 == 4");
    expect(run_stmt(ctx, 271U, {750}), "271 battle_size 750");
    expect(run_stmt(ctx, 271U, {10001}), "271 OOB ignored");
    expect(run_stmt(ctx, 270U, {reg(6)}), "270 get battle_size");
    expect(ctx.registers[6] == 750, "270 == 750");

    // 350/351 profile banner
    ctx.world.mp.current_profile_no = 1;
    expect(run_stmt(ctx, 351U, {14}), "351 profile_set_banner_id");
    expect(run_stmt(ctx, 350U, {reg(7)}), "350 profile_get_banner_id");
    expect(ctx.registers[7] == 14, "350 == 14");

    // 380 send_message_to_url: raw vs selective token URL-encoding & cap
    ctx.stringRegisters[0] = u"https://example.com/hit?name={s1}";
    ctx.stringRegisters[1] = u"hello world&x=1";
    expect(run_stmt(ctx, 380U, {0, 0}), "380 unencoded");
    expect(run_stmt(ctx, 380U, {0, 1}), "380 token-encoded");
    expect(ctx.world.mp.url_requests.size() == 2U, "380 queued 2 requests");
    expect(ctx.world.mp.url_requests[0].raw &&
               ctx.world.mp.url_requests[0].url ==
                   "https://example.com/hit?name=hello world&x=1",
           "380 raw preserves spaces");
    expect(!ctx.world.mp.url_requests[1].raw &&
               ctx.world.mp.url_requests[1].url ==
                   "https://example.com/hit?name=hello%20world%26x%3D1",
           "380 encodes token while preserving URL delimiters");
    for (int i = 0; i < 150; ++i) {
        expect(run_stmt(ctx, 380U, {0, 0}), "380 bounded loop");
    }
    expect(ctx.world.mp.url_requests.size() == wse::game::kMaxUrlRequests,
           "380 respects kMaxUrlRequests cap");

    // 426/427 spawn effectors
    expect(run_stmt(ctx, 426U, {3, 4}), "426 set prop kind");
    expect(run_stmt(ctx, 426U, {8, 4}), "426 bad team ignored");
    expect(ctx.world.fx.spawn_effector_scene_prop_kinds[3] == 4, "426 stored");
    expect(run_stmt(ctx, 427U, {19, 1}), "427 slot 19");
    expect(run_stmt(ctx, 427U, {20, 1}), "427 slot 20 OOB ignored");
    expect(ctx.world.fx.spawn_effector_mission_object_nos[19] == 1, "427 stored");

    // 465..468 kick & bans
    ctx.world.is_server = false;
    expect(run_stmt(ctx, 465U, {1}), "465 non-server no-op");
    expect(ctx.world.players[1].status == 2, "465 non-server keeps active");
    ctx.world.is_server = true;
    expect(run_stmt(ctx, 465U, {0}), "465 player 0 protected");
    expect(run_stmt(ctx, 465U, {2}), "465 privilege >= 3 protected");
    expect(ctx.world.players[0].status == 2 && ctx.world.players[2].status == 2,
           "465 host & admin stay active");
    expect(run_stmt(ctx, 467U, {1}), "467 save_ban_info_of_player");
    expect(ctx.world.mp.saved_ban.valid && ctx.world.mp.saved_ban.player_no == 1,
           "467 saved ban info");
    expect(run_stmt(ctx, 468U, {}), "468 ban_player_using_saved_ban_info");
    expect(!ctx.world.mp.saved_ban.valid &&
               ctx.world.mp.banned_players.size() == 1U &&
               ctx.world.players[1].status == 0,
           "468 consumed saved ban, recorded ban, kicked player 1");
    ctx.world.players[1].status = 2;
    expect(run_stmt(ctx, 466U, {1, 0, 3}), "466 temp ban with banner player 3");
    expect(ctx.world.mp.banned_players.size() == 2U &&
               !ctx.world.mp.banned_players[1].permanent &&
               ctx.world.mp.banned_players[1].banner_player_no == 3 &&
               ctx.world.players[1].status == 0,
           "466 recorded temp ban and kicked");
    ctx.world.players[1].status = 2;
    for (int i = 0; i < 530; ++i) {
        ctx.world.players[1].status = 2;
        expect(run_stmt(ctx, 466U, {1, 1, -1}), "466 cap fill");
    }
    expect(ctx.world.mp.banned_players.size() == wse::game::kMaxBans,
           "466 enforces kMaxBans cap");

    // 470 start_multiplayer_mission
    ctx.world.is_multiplayer = true;
    ctx.randSeed = 12345;
    expect(run_stmt(ctx, 470U, {1, 2, 1}), "470 start_multiplayer_mission");
    expect(ctx.world.mp.mission_starting && ctx.world.mp.started_manually &&
               ctx.world.mp.next_mission_template_no == 1 &&
               ctx.world.mp.next_site_no == 2 && ctx.randSeed != 12345,
           "470 set state and randomized mission seed");
}

void test_ops3_audio_pos_ui_and_difficulty() {
    wse::vm::VmContext ctx;
    init_world(ctx);

    // 616 stop_sound_channel
    expect(run_stmt(ctx, 616U, {-1}), "616 negative channel ignored");
    expect(run_stmt(ctx, 616U, {9999999}), "616 huge channel ignored");
    expect(run_stmt(ctx, 616U, {5}), "616 channel 5 stopped");
    expect(ctx.world.music.stopped_channels.size() == 6U &&
               ctx.world.music.stopped_channels[5],
           "616 marked channel 5 stopped");

    // 702, 1950, 1955, 715
    ctx.world.fx.trigger_transform.o = {1.0f, 2.0f, 3.0f, 1.0f};
    expect(run_stmt(ctx, 702U, {0}), "702 get_trigger_object_position");
    expect(close(ctx.positions[0].o.x, 1.0f) && close(ctx.positions[0].o.z, 3.0f),
           "702 copied trigger transform");
    expect(run_stmt(ctx, 1950U, {50, 100, 25}), "1950 set_current_color");
    expect(close(ctx.world.fx.trigger_current_color.x, 0.5f) &&
               close(ctx.world.fx.trigger_current_color.y, 1.0f) &&
               close(ctx.world.fx.trigger_current_color.w, 1.0f),
           "1950 stored RGBA");
    expect(run_stmt(ctx, 1955U, {150, -50, 25}), "1955 set_position_delta");
    expect(close(ctx.world.fx.trigger_position_delta.x, 1.5f) &&
               close(ctx.world.fx.trigger_position_delta.y, -0.5f) &&
               close(ctx.world.fx.trigger_position_delta.w, 0.0f),
           "1955 stored XYZ0");

    ctx.positions[1].o.z = 5.0f;
    ctx.positions[2].o.z = 2.0f;
    expect(run_stmt(ctx, 715U, {reg(0), 1, 2}), "715 height sq diff");
    expect(ctx.registers[0] == 900, "715 (5-2)^2 * 100 = 900");
    expect(run_stmt(ctx, 715U, {reg(1), -1, 2}), "715 bad reg -> 0");
    expect(ctx.registers[1] == 0, "715 bad reg zeroed");
    ctx.positions[1].o.z = std::numeric_limits<float>::quiet_NaN();
    expect(run_stmt(ctx, 715U, {reg(2), 1, 2}), "715 NaN safe -> 0");
    expect(ctx.registers[2] == 0, "715 NaN zeroed");

    // 960, 970..972, 980, 985, 988
    expect(run_stmt(ctx, 960U, {0}), "960 hide object details overlay");
    expect(!ctx.world.ui.show_object_details_overlay, "960 false");
    ctx.positions[3].o = {300.0f, 400.0f, 0.0f, 1.0f};
    expect(run_stmt(ctx, 972U, {2, 12, 3, 1}), "972 show item details with mod 12");
    expect(ctx.world.ui.item_tooltip.visible &&
               ctx.world.ui.item_tooltip.modifier == 12 &&
               close(ctx.world.ui.item_tooltip.screen_pos.x, 300.0f),
           "972 keeps mod < 43");
    expect(run_stmt(ctx, 972U, {2, 99, 3, 1}), "972 mod >= 43 defaults to 0");
    expect(ctx.world.ui.item_tooltip.modifier == 0, "972 mod clamped to 0");
    expect(run_stmt(ctx, 971U, {}), "971 close_item_details");
    expect(!ctx.world.ui.item_tooltip.visible, "971 hid item tooltip");
    expect(run_stmt(ctx, 970U, {1, 3, 2}), "970 show_item_details");
    expect(ctx.world.ui.item_tooltip.visible && ctx.world.ui.item_tooltip.item_no == 1,
           "970 shows item");

    ctx.stringRegisters[0] = u"Entry {reg3}";
    ctx.registers[3] = 7;
    for (int i = 0; i < 120; ++i) {
        expect(run_stmt(ctx, 980U, {0, i}), "980 context_menu_add_item");
    }
    expect(ctx.world.ui.context_menu_items.size() == wse::game::kMaxContextMenuItems &&
               ctx.world.ui.context_menu_items[0].text == "Entry 7",
           "980 parsed text and capped at 100");
    expect(run_stmt(ctx, 985U, {}), "985 auto_save");
    expect(run_stmt(ctx, 988U, {1}), "988 allow_ironman");
    expect(ctx.world.misc.auto_save_count == 1 && ctx.world.misc.ironman_allowed,
           "985 & 988 updated");

    // 990 get_average_game_difficulty: t=0, save_mode_flags, t>0 progression, 0% clamp
    ctx.timersTotal[0] = 0;
    expect(run_stmt(ctx, 990U, {reg(4)}), "990 t=0 default");
    expect(ctx.registers[4] == 79, "990 default options yield 79%");

    wse::vm::VmContext save_ctx;
    init_world(save_ctx);
    save_ctx.world.misc.save_mode_flags = 1;
    expect(run_stmt(save_ctx, 990U, {reg(0)}), "990 save_mode_flags != 0");
    expect(save_ctx.registers[0] == 59, "990 save mode scales by 0.75 -> 59%");

    wse::vm::VmContext prog_ctx;
    init_world(prog_ctx);
    prog_ctx.options.combat_ai = 0;
    prog_ctx.options.campaign_ai = 0;
    prog_ctx.options.damage_to_player = 1.0f;
    prog_ctx.options.damage_to_friends = 1.0f;
    prog_ctx.options.combat_speed = 2;
    prog_ctx.options.battle_size = 0.5f;  // 1.0 * 1.0 * 1.0 * 1.0 * 1.0 * 1.0 * 0.95 * 1.0 * 100.01 = 95
    prog_ctx.timersTotal[0] = 1000000;    // t = 10s
    expect(run_stmt(prog_ctx, 990U, {reg(0)}), "990 t=10s");
    expect(prog_ctx.registers[0] == 95, "990 first window -> 95");
    prog_ctx.options.campaign_ai = 2;     // * 0.70 -> ~66
    prog_ctx.timersTotal[0] = 2000000;    // t = 20s (+10s -> 50/50 avg of 95 & 66 = 80)
    expect(run_stmt(prog_ctx, 990U, {reg(1)}), "990 t=20s");
    expect(prog_ctx.registers[1] == 80, "990 weighted avg -> 80");

    wse::vm::VmContext clamp_ctx;
    init_world(clamp_ctx);
    clamp_ctx.options.damage_to_friends = -5.0f;
    clamp_ctx.options.damage_to_player = std::numeric_limits<float>::quiet_NaN();
    expect(run_stmt(clamp_ctx, 990U, {reg(0)}), "990 extreme negative / NaN clamp");
    expect(clamp_ctx.registers[0] == 0, "990 clamped cleanly to 0%");

    // 991 get_level_boundary
    expect(run_stmt(ctx, 991U, {reg(5), 1}), "991 level 1 boundary");
    expect(ctx.registers[5] == 600, "991 level 1 = 600");
    ctx.registers[5] = -7;
    expect(run_stmt(ctx, 991U, {reg(5), 64}), "991 OOB level ignored");
    expect(ctx.registers[5] == -7, "991 destination preserved");
}

void test_ops3_dialog_quests_encounters() {
    wse::vm::VmContext ctx;
    init_world(ctx);

    // 1025 start_map_conversation
    expect(run_stmt(ctx, 1025U, {2, 0, 1, 99, 0}), "1025 custom dna & active");
    expect(ctx.world.dialog.map_conversation_troop_no == 2 &&
               ctx.world.dialog.map_conversation_troop_dna == 99 &&
               ctx.world.dialog.map_conversation_active,
           "1025 state set");

    // 1090..1092, 1109, 1111, 1112, 1115, 1118, 1130
    expect(run_stmt(ctx, 1090U, {0, 3}), "1090 info page note tableau");
    expect(run_stmt(ctx, 1109U, {1, 4}), "1109 faction note tableau");
    expect(run_stmt(ctx, 1111U, {2, 5}), "1111 quest note tableau");
    expect(ctx.world.info_pages[0].note_tableau_material_no == 3 &&
               ctx.world.factions[1].note_tableau_material_no == 4 &&
               ctx.world.quests[2].note_tableau_material_no == 5,
           "note tableau materials assigned");

    ctx.world.current_day = 17;
    expect(run_stmt(ctx, 1091U, {0, 1, 1}, 0), "1091 caller != 4 ignored");
    expect(ctx.world.info_pages[0].notes[1].text.empty(), "1091 not written when caller != 4");
    expect(run_stmt(ctx, 1091U, {0, 1, 1}, 4), "1091 caller == 4");
    expect(run_stmt(ctx, 1112U, {1, 2, 0}, 4), "1112 quest note from dialog");
    expect(run_stmt(ctx, 1115U, {2, 3, 1}, 4), "1115 faction note from dialog");
    expect(!ctx.world.info_pages[0].notes[1].text.empty() &&
               ctx.world.info_pages[0].notes[1].date == ctx.world.current_day &&
               ctx.world.quests[1].notes[2].date == -1 &&
               !ctx.world.factions[2].notes[3].text.empty(),
           "dialog notes populated with expected current_day dates");

    ctx.stringRegisters[2] = u"Note sreg";
    expect(run_stmt(ctx, 1092U, {0, 4, 2, 0}), "1092 info page note from sreg");
    expect(run_stmt(ctx, 1118U, {1, 5, 2, 1}), "1118 faction note from sreg");
    expect(ctx.world.info_pages[0].notes[4].text == u"Note sreg" &&
               ctx.world.info_pages[0].notes[4].date == -1 &&
               ctx.world.factions[1].notes[5].date == ctx.world.current_day,
           "sreg notes stored with expected current_day dates");

    ctx.stringRegisters[0] = u"TipText";
    expect(run_stmt(ctx, 1130U, {0}), "1130 set_tooltip_text");
    expect(ctx.world.ui.tooltip_text == "TipText", "1130 tooltip updated");

    // 1285, 1290, 1291
    ctx.world.quests[0].progression_flags = 0x300U;
    expect(run_stmt(ctx, 1285U, {0, 0x1a5}), "1285 low byte only");
    expect(ctx.world.quests[0].progression_flags == 0x3a5U, "1285 preserves bits >= 8");

    ctx.world.translations["qst_test_text"] = "Translated {s3}";
    ctx.stringRegisters[3] = u"QuestBody";
    ctx.registers[0] = 9;
    expect(run_stmt(ctx, 1290U, {0}), "1290 setup_quest_text");
    expect(ctx.world.quests[0].text == "Translated QuestBody" &&
               ctx.world.misc.quest_strings[0].text == "Translated QuestBody" &&
               ctx.world.quests[0].title == "Fallback 9 Title",
           "1290 resolved translation/fallback + tokens");
    ctx.stringRegisters[1] = u"Lord Giver";
    expect(run_stmt(ctx, 1291U, {0, 1}), "1291 setup_quest_giver");
    expect(ctx.world.misc.quest_strings[0].giver_name == "Lord Giver", "1291 giver set");

    // 1300 start_encounter: root traversal, cycle guard, main-party attacker swap
    ctx.world.parties[2].attached_to_party_no = 1;
    ctx.world.parties[3].attached_to_party_no = 4;
    ctx.world.parties[4].attached_to_party_no = 3;  // cycle!
    expect(run_stmt(ctx, 1300U, {2, 0}), "1300 party 2 encountered via camp default");
    expect(ctx.world.encountered_party_no == 1 && !ctx.world.encountered_party_is_attacker &&
               ctx.world.misc.repeat_object == 0 && ctx.world.encountered_party_2_no == -1,
           "1300 resolved attached root party 1 as defender");

    ctx.world.misc.camp_bandits_party_no = 0;
    expect(run_stmt(ctx, 1300U, {2, 0}), "1300 camp_bandits_party_no=0 stays defender");
    expect(!ctx.world.encountered_party_is_attacker && ctx.world.misc.repeat_object == 0,
           "1300 camp_bandits match does not swap attacker");

    ctx.world.misc.camp_bandits_party_no = 4;
    expect(run_stmt(ctx, 1300U, {2, 0}), "1300 main party attacks party 2 (narrow[1]=0)");
    expect(!ctx.world.encountered_party_is_attacker, "narrow[1] < 1 picks camp_bandits=4 (root)");

    // Passing main_party_no = 0 triggers main-party-attacker path when main_party_no != camp_bandits:
    // use party 1 as main_party_no (> 0) for direct explicit main attacker operand test.
    ctx.world.parties[2].attached_to_party_no = -1;
    ctx.world.main_party_no = 1;
    ctx.world.misc.camp_bandits_party_no = -1;
    expect(run_stmt(ctx, 1300U, {2, 1}), "1300 explicit main_party_no attacker");
    expect(ctx.world.encountered_party_no == 1 && ctx.world.encountered_party_is_attacker &&
               ctx.world.misc.repeat_object == 1,
           "1300 swaps attacker when p1 == main_party_no");

    expect(run_stmt(ctx, 1300U, {3, 2}), "1300 cyclic attached_to_party_no terminates");

    // 1301, 1303..1305, 1307
    expect(run_stmt(ctx, 1301U, {}), "1301 leave_encounter");
    expect(ctx.world.misc.encounter_left && ctx.world.misc.save_mode_flags == 0,
           "1301 records encounter_left without touching save_mode_flags");
    expect(run_stmt(ctx, 1303U, {1}), "1303 select_enemy(1)");
    expect(ctx.world.main_party_side == 0, "1303 side=0");
    expect(run_stmt(ctx, 1304U, {2}), "1304 set_passage_menu");
    expect(ctx.world.misc.passage_menu_no == 2, "1304 passage_menu_no=2");
    expect(run_stmt(ctx, 1305U, {}), "1305 meta_mission_end_committed");
    expect(run_stmt(ctx, 1307U, {}), "1307 end_current_battle");
    expect(!ctx.world.misc.meta_mission_end_committed &&
               ctx.world.misc.meta_mission_event_ended,
           "1305 & 1307 updated");
}

void test_ops3_mission_fx_ui_and_lighting() {
    wse::vm::VmContext ctx;
    init_world(ctx);

    // 1805 ai_mesh_face_group_show_hide
    ctx.world.fx.ai_mesh_faces = {{5, true}, {6, true}, {5, true}};
    expect(run_stmt(ctx, 1805U, {5, 0}), "1805 hide group 5");
    expect(!ctx.world.fx.ai_mesh_faces[0].visible &&
               ctx.world.fx.ai_mesh_faces[1].visible &&
               !ctx.world.fx.ai_mesh_faces[2].visible,
           "1805 toggled group 5 only");

    // 1829 add_missile + cap
    expect(run_stmt(ctx, 1829U, {1, 0, 2500, 2, 10, 3, 99}), "1829 add_missile");
    expect(ctx.world.fx.missiles.size() == 1U &&
               ctx.world.fx.missiles[0].item_modifier == 10 &&
               ctx.world.fx.missiles[0].missile_modifier == 0 &&
               close(ctx.world.fx.missiles[0].speed, 2.5f),
           "1829 missile fields and modifier clamping");
    for (int i = 0; i < 1100; ++i) {
        expect(run_stmt(ctx, 1829U, {1, 0, 1000, 2, 0, 3, 0}), "1829 cap loop");
    }
    expect(ctx.world.fx.missiles.size() == 1024U, "1829 capped at 1024");

    // 1891 replace_scene_items_with_scene_props (caller -20 or -19 only)
    expect(run_stmt(ctx, 1891U, {1, 4}, 0), "1891 wrong caller ignored");
    expect(ctx.world.mission_objects[0].is_item &&
               ctx.world.mission_objects[0].scene_prop_no == 1,
           "1891 unchanged");
    expect(run_stmt(ctx, 1891U, {1, 4}, -20), "1891 caller -20");
    expect(!ctx.world.mission_objects[0].is_item &&
               ctx.world.mission_objects[0].scene_prop_no == 4 &&
               ctx.world.mission_objects[1].scene_prop_no == 1,
           "1891 replaced scene item only");

    // 1920, 1982, 1983, 1986
    expect(run_stmt(ctx, 1920U, {2}), "1920 start_mission_conversation");
    expect(ctx.world.dialog.pending_mission_agent_no == 1, "1920 found agent 1");
    expect(run_stmt(ctx, 1982U, {3, 44}, -40), "1982 cur_scene_prop_set_tableau_material");
    expect(ctx.world.fx.cur_scene_prop_tableau_material_no == 3 &&
               ctx.world.fx.cur_scene_prop_tableau_param == 44,
           "1982 set");
    expect(run_stmt(ctx, 1983U, {4, 55}, -70), "1983 cur_map_icon_set_tableau_material");
    expect(ctx.world.fx.cur_map_icon_tableau_material_no == 4 &&
               ctx.world.fx.cur_map_icon_tableau_param == 55,
           "1983 set");
    ctx.world.fx.trigger_agent_no = 1;
    expect(run_stmt(ctx, 1986U, {5, 66}), "1986 cur_agent_set_banner_tableau_material");
    expect(ctx.world.agents[1].banner_tableau_material_no == 5 &&
               ctx.world.agents[1].banner_tableau_param == 66 &&
               ctx.world.agents[1].show_banner,
           "1986 set agent banner");

    // 2020..2022, 2031, 2032, 2061, 2198
    expect(run_stmt(ctx, 2021U, {15}), "2021 relation bar");
    ctx.stringRegisters[1] = u"Talk Line";
    expect(run_stmt(ctx, 2022U, {2, 1}), "2022 talk_info_set_line");
    expect(ctx.world.dialog.talk_info_lines[2] == "Talk Line" &&
               ctx.world.dialog.show_talk_info &&
               ctx.world.dialog.talk_info_relation_value == 15,
           "2021 & 2022 set");
    expect(run_stmt(ctx, 2020U, {0}), "2020 talk_info_show(0)");
    expect(!ctx.world.dialog.show_talk_info, "2020 disabled");

    ctx.world.ui.current_menu_no = 1;
    expect(run_stmt(ctx, 2031U, {1}), "2031 set_background_mesh");
    ctx.positions[2].o = {0.1f, 0.2f, 0.3f, 0.4f};
    expect(run_stmt(ctx, 2032U, {2, 7, 2}), "2032 set_game_menu_tableau_mesh");
    expect(run_stmt(ctx, 2061U, {}), "2061 disable_menu_option");
    expect(run_stmt(ctx, 2198U, {2}), "2198 set_conversation_speaker_agent");
    expect(ctx.world.ui.background_mesh_name == "mesh_bg" &&
               ctx.world.ui.menu_tableau_material_no == 2 &&
               close(ctx.world.ui.menu_tableau_param, 7.0f) &&
               close(ctx.world.ui.menu_tableau_bounds.z, 0.3f) &&
               ctx.world.ui.current_menu_option_disabled &&
               ctx.world.dialog.speaker_agent_no == 2,
           "2031..2198 updated");

    // 2386, 2387, 2389..2396 lighting/skybox/postfx
    expect(run_stmt(ctx, 2386U, {2}), "2386 set_postfx");
    expect(run_stmt(ctx, 2387U, {}), "2387 set_river_shader_to_mud");
    expect(run_stmt(ctx, 2389U, {3, 9}), "2389 set_skybox");
    expect(run_stmt(ctx, 2390U, {500, 600, 700}), "2390 sun");
    expect(run_stmt(ctx, 2391U, {100, 200, 300}), "2391 ambient");
    expect(run_stmt(ctx, 2392U, {400, 500, 600}), "2392 ground ambient");
    expect(run_stmt(ctx, 2393U, {}), "2393 rebuild_shadow_map");
    expect(run_stmt(ctx, 2394U, {0}), "2394 get sun");
    expect(run_stmt(ctx, 2395U, {1}), "2395 get ambient");
    expect(run_stmt(ctx, 2396U, {3}), "2396 get ground ambient");
    expect(ctx.world.lighting.active_postfx_no == 2 &&
               ctx.world.lighting.river_shader_mud &&
               ctx.world.lighting.skybox_no == 3 &&
               ctx.world.lighting.hdr_skybox_no == 9 &&
               ctx.world.lighting.shadow_map_needs_restore &&
               close(ctx.positions[0].o.x, 0.5f) &&
               close(ctx.positions[1].o.y, 0.2f) &&
               close(ctx.positions[3].o.z, 0.6f),
           "lighting values round-tripped");

    ctx.world.lighting.has_ambient_light = false;
    ctx.world.lighting.has_scene = false;
    ctx.positions[1].o.y = 9.0f;
    expect(run_stmt(ctx, 2394U, {0}), "2394 without ambient_light clears o");
    expect(run_stmt(ctx, 2395U, {1}), "2395 without scene leaves o untouched");
    expect(close(ctx.positions[0].o.x, 0.0f) && close(ctx.positions[1].o.y, 9.0f),
           "2394/2395 gated cleanly on scene flags");

    // INT64_MIN boundary inputs
    const std::int64_t imin = std::numeric_limits<std::int64_t>::min();
    expect(run_stmt(ctx, 616U, {imin}), "616 INT64_MIN safe");
    expect(run_stmt(ctx, 991U, {reg(0), imin}), "991 INT64_MIN safe");
    expect(run_stmt(ctx, 1300U, {imin, imin}), "1300 INT64_MIN safe");
    expect(run_stmt(ctx, 1829U, {imin, imin, imin, imin, imin, imin, imin}),
           "1829 INT64_MIN safe");
}

// Regressions found in the Milestone 4 review.
void test_m4_review_regressions() {
    using wse::vm::VmContext;

    // 1301 leave_encounter must not change the computed game difficulty.
    {
        VmContext ctx;
        const std::int32_t before = wse::vm::native_world_current_difficulty(ctx);
        expect(run_stmt(ctx, 1301U, {}), "1301 leave_encounter");
        expect(wse::vm::native_world_current_difficulty(ctx) == before,
               "1301 leaves game difficulty unchanged");
    }

    // 1490 set_merchandise_modifier_quality keeps the fraction.
    {
        VmContext ctx;
        expect(run_stmt(ctx, 1490U, {150}), "1490 quality 150");
        expect(close(ctx.world.misc.merchandise_modifier_quality, 1.5f),
               "1490 stores 150 as 1.5, not 1");
        expect(run_stmt(ctx, 1490U, {50}), "1490 quality 50");
        expect(close(ctx.world.misc.merchandise_modifier_quality, 0.5f),
               "1490 stores 50 as 0.5, not 0");
    }

    // 1971 spawn_item marks the object as a spawned item, so the spawned
    // instance queries (1832/1833) see it and the scene-item ones (1830) do not.
    {
        VmContext ctx;
        ctx.world.items.resize(3);
        expect(run_stmt(ctx, 1971U, {1, 0, 0}), "1971 spawn_item");
        const std::int64_t item_obj = ctx.registers[0];
        expect(item_obj == 0 && ctx.world.mission_objects[0].m5.is_spawned_item,
               "1971 flags the object as a spawned item");
        expect(run_stmt(ctx, 1832U, {reg(5), 1}) && ctx.registers[5] == 1,
               "1832 counts the spawned item");
        expect(run_stmt(ctx, 1830U, {reg(6), 1}) && ctx.registers[6] == 0,
               "1830 does not count it as a scene item");
        expect(run_stmt(ctx, 1833U, {reg(7), 1, 0}) && ctx.registers[7] == item_obj,
               "1833 finds the spawned item instance");
    }

    // Phase 2E player container lifecycle (`mbnetPlayer::initialize`/`clear`/`disconnectPlayer`).
    {
        VmContext pctx;
        auto& w = pctx.world;
        w.is_server = true;
        w.init_players(4);
        w.agents.resize(1);
        w.agents[0].active = true;
        w.parties.resize(1);
        w.parties[0].active = true;

        expect(w.players.size() == 4U && w.players[1].status == 0 &&
                   w.players[1].name == "UNASSIGNED" && w.players[1].team_no == 2,
               "init_players initializes fixed-capacity UNASSIGNED team-2 slots");
        expect(w.connect_player(1, "Sir_Robin", 4242, 0), "connect_player activates slot 1");
        w.players[1].agent_no = 0;
        w.agents[0].player_no = 1;
        w.agents[0].is_non_player = false;
        w.players[1].gold = 950;
        w.players[1].score = 12;
        w.players[1].kill_count = 3;
        w.players[1].death_count = 1;
        w.players[1].banner_no = 5;
        w.players[1].troop_no = 2;
        w.players[1].item_ids[0] = 9;
        w.players[1].picked_up_slots[0] = true;
        w.players[1].slots.set(0, 88);
        w.m6.player(1).party_no = 0;
        w.parties[0].player_no = 1;

        expect(run_stmt(pctx, 465U, {1}), "465 kick_player runs retail disconnect");
        expect(!w.is_valid_player(1) && w.players[1].status == 0 &&
                   w.players[1].name == "UNASSIGNED" && w.players[1].team_no == 2 &&
                   w.players[1].agent_no == -1 && w.players[1].gold == 0 &&
                   w.players[1].score == 0 && w.players[1].kill_count == 0 &&
                   w.players[1].death_count == 0 && w.players[1].banner_no == -1 &&
                   w.players[1].troop_no == -1 && w.players[1].item_ids[0] == -1 &&
                   !w.players[1].picked_up_slots[0] &&
                   w.players[1].slots.get(0) == 0 &&
                   w.agents[0].player_no == -1 && w.agents[0].is_non_player &&
                   w.parties[0].player_no == -1,
               "kick_player clears scores/items/slots/name/team and unlinks agent & party");
    }

    // Review regression (e6c4d0e): saved ban info outlives the player's connection, so a ban
    // poll that ends after the player was kicked still bans them.
    {
        VmContext bctx;
        auto& w = bctx.world;
        w.is_server = true;
        w.init_players(4);
        expect(w.connect_player(2, "Griefer", 9001, 0), "connect_player activates slot 2");
        expect(run_stmt(bctx, 467U, {2}) && w.mp.saved_ban.valid,
               "467 save_ban_info_of_player stores the record");
        expect(run_stmt(bctx, 465U, {2}) && !w.is_valid_player(2) && w.mp.saved_ban.valid,
               "465 kick_player keeps the saved ban record");
        const std::size_t bans_before = w.mp.banned_players.size();
        expect(run_stmt(bctx, 468U, {}) && w.mp.banned_players.size() == bans_before + 1U &&
                   w.mp.banned_players.back().unique_id == 9001 && !w.mp.saved_ban.valid,
               "468 ban_player_using_saved_ban_info bans the kicked player");
    }
}

}  // namespace

namespace wse::selftest {

int run_native_world_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_m4_review_regressions();
    test_world_ops_1();
    test_world_ops_2_and_non_switch_and_m3_upgrades();
    test_ops3_options_profile_http_mod();
    test_ops3_audio_pos_ui_and_difficulty();
    test_ops3_dialog_quests_encounters();
    test_ops3_mission_fx_ui_and_lighting();

    if (g_failures == 0) {
        std::printf("PASS  native_world_test\n");
        return 0;
    }
    std::printf("FAIL  native_world_test (%d failures)\n", g_failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_native_world_suite(nullptr);
}
#endif
