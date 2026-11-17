// Host unit tests for Milestone 5 native opcodes across agents, multiplayer &
// players, scenes/props/lighting/camera/shaders, and UI/presentations/menus.
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

void init_m5_world(wse::vm::VmContext& ctx) {
    auto& w = ctx.world;
    w.has_game = true;
    ctx.fixedPointMultiplier = 1000;

    w.troops.resize(4);
    w.troops[0].name = "Hero";
    w.troops[1].name = "Soldier";
    w.troops[2].name = "Guard";

    w.items.resize(4);
    w.items[0].value = 100;
    w.items[0].hit_points = 80;
    w.items[1].properties = 0x07ULL;  // shield (type 7)
    w.items[1].hit_points = 120;
    w.items[1].value = 250;
    w.items[2].properties = 0x02ULL;  // one-handed weapon (type 2)
    w.items[2].value = 500;
    w.items[3].hit_points = 160;
    w.items[3].value = 900;

    w.parties.resize(4);
    w.main_party_no = 0;
    w.factions.resize(3);
    w.quests.resize(2);
    w.info_pages.resize(2);
    w.mesh_names = {"mesh_zero", "mesh_btn", "mesh_pressed"};
    w.misc.num_tableau_materials = 4;
    w.misc.num_scene_props = 6;
    w.misc.num_actions = 600;
    w.num_particle_systems = 4;
    w.lighting.has_scene = true;

    w.players.resize(4);
    w.players[0].name = "Host";
    w.players[0].unique_id = 0x01234567;
    w.players[0].profile_gender = 0;
    w.players[0].troop_no = 1;
    w.players[0].agent_no = 0;
    w.players[1].name = "Guest";
    w.players[1].unique_id = 0x76543210;
    w.players[1].profile_gender = 1;
    w.players[1].score = 42;
    w.players[1].kill_count = 7;
    w.players[1].death_count = 2;
    w.players[1].ping = 35;

    w.agents.resize(4);
    for (auto& ag : w.agents) {
        ag.active = true;
        ag.status = 1;
        ag.type = 0;
        ag.is_non_player = true;
        ag.hit_points = 100.0f;
        ag.max_hit_points = 100.0f;
        ag.damage_modifier = 1.0f;
        ag.position = wse::core::WseTransform::identity();
    }
    w.agents[0].team_no = 0;
    w.agents[0].entry_no = 3;
    w.agents[1].team_no = 0;
    w.agents[1].entry_no = 5;
    w.agents[2].team_no = 1;
    w.agents[2].entry_no = 12;
    w.agents[3].team_no = 1;
    w.agents[3].type = 1;  // horse

    w.mission_objects.resize(4);
    for (auto& obj : w.mission_objects) {
        obj.active = true;
        obj.is_item = false;
        obj.scene_prop_no = 1;
        obj.hit_points = 100.0f;
        obj.max_hit_points = 150.0f;
        obj.scale = {1.0f, 1.0f, 1.0f, 1.0f};
        obj.position = wse::core::WseTransform::identity();
        obj.m5.bound_half_extents = {1.0f, 1.0f, 1.0f, 0.0f};
    }
}

void test_agents_m5() {
    wse::vm::VmContext ctx;
    init_m5_world(ctx);
    auto& w = ctx.world;

    // 1689 agent_get_speed & 1717 agent_get_entry_no
    w.agents[1].m5.speed = {3.5f, 1.25f};
    expect(run_stmt(ctx, 1689U, {0, 1}), "1689 agent_get_speed");
    expect(close(ctx.positions[0].o.x, 3.5f) && close(ctx.positions[0].o.y, 1.25f),
           "1689 reads m5 speed");
    expect(run_stmt(ctx, 1717U, {reg(0), 2}), "1717 agent_get_entry_no");
    expect(ctx.registers[0] == 12, "1717 returns entry_no 12");

    // 1692 agent_refill_wielded_shield_hit_points
    w.agents[1].equipped_items[1] = 1;
    w.agents[1].wielded_items[1] = 1;
    w.agents[1].m5.item_hit_points[1] = 5;
    expect(run_stmt(ctx, 1692U, {1}), "1692 refill shield hp");
    expect(w.agents[1].m5.item_hit_points[1] == 120, "1692 sets hp to item max");

    // 1693, 1713, 1725, 1733, 1734, 1739, 1742..1749, 1752..1754
    expect(!run_stmt(ctx, 1693U, {1}), "1693 false before scripted mode");
    ctx.positions[1] = wse::core::WseTransform::identity();
    ctx.positions[1].o = {10.0f, 20.0f, 5.0f, 1.0f};
    w.ground_level_z = 2.0f;
    expect(run_stmt(ctx, 1748U, {1, 1, 1, 0}), "1748 scripted dest no attack");
    expect(run_stmt(ctx, 1693U, {1}), "1693 true after scripted mode");
    expect(close(w.agents[1].target_position.o.z, 2.0f), "1748 clamps z to ground");

    expect(run_stmt(ctx, 1713U, {1, 2}), "1713 look target agent");
    expect(run_stmt(ctx, 2080U, {reg(1), 1}), "2080 get look target");
    expect(ctx.registers[1] == 2 && w.agents[1].m5.look_target_flags == 2,
           "1713/2080 round-trip");
    expect(run_stmt(ctx, 1744U, {1, 1}), "1744 look target pos");
    expect(w.agents[1].m5.look_target_flags == 1 &&
               close(w.agents[1].m5.look_target_pos.x, 10.0f),
           "1744 sets target pos");

    expect(run_stmt(ctx, 1725U, {1, 1}), "1725 invulnerable shield");
    expect(run_stmt(ctx, 1733U, {1, 1}), "1733 no death knockdown only");
    expect(run_stmt(ctx, 1734U, {1, 150}), "1734 horse speed factor");
    expect(run_stmt(ctx, 1742U, {1, 42}), "1742 walk forward anim");
    expect(run_stmt(ctx, 1743U, {1, 250, 1}), "1743 anim progress");
    expect(run_stmt(ctx, 1745U, {1, 2, 1}), "1745 attack action override");
    expect(run_stmt(ctx, 1746U, {1, 3, 500}), "1746 defend action override");
    expect(run_stmt(ctx, 1753U, {1, 800}), "1753 aggressiveness");
    expect(run_stmt(ctx, 1754U, {1, 1}), "1754 kick allowed");
    expect(run_stmt(ctx, 1762U, {1, 1}), "1762 no dynamics");
    expect(w.agents[1].m5.invulnerable_shield &&
               w.agents[1].m5.no_death_knockdown_only &&
               close(w.agents[1].m5.horse_speed_factor, 1.5f) &&
               w.agents[1].m5.walk_action_no == 42 &&
               close(w.agents[1].m5.action_channels[1].progress, 0.25f) &&
               w.agents[1].m5.attack_override_type == 2 &&
               w.agents[1].m5.attack_override_hold == 1 &&
               w.agents[1].m5.defend_override_type == 3 &&
               close(w.agents[1].m5.defend_override_duration, 0.5f) &&
               close(w.agents[1].m5.aggressiveness, 0.8f) &&
               w.agents[1].m5.can_kick && w.agents[1].m5.no_dynamics,
           "agent setter flags updated");

    // 1749 agent_fade_out & 1752 agent_stop_running_away
    w.agents[1].m5.exit_status = 2;
    expect(run_stmt(ctx, 1749U, {1}), "1749 fade out");
    expect(w.agents[1].m5.exit_status == 3 && w.agents[1].running_away, "1749 activated");
    expect(run_stmt(ctx, 1752U, {1}), "1752 stop running away");
    expect(!w.agents[1].running_away && w.agents[1].m5.behavior == 4, "1752 cleared");

    // 1757..1759, 1809 attached scene prop & offsets
    expect(run_stmt(ctx, 1757U, {1, 2, 0, 4, 1}), "1757 attach scene prop");
    expect(run_stmt(ctx, 1758U, {1, 7, 0}), "1758 attach x");
    expect(run_stmt(ctx, 1809U, {1, 8, 0}), "1809 attach y");
    expect(run_stmt(ctx, 1759U, {1, 9, 0}), "1759 attach z");
    expect(w.agents[1].attached_mission_object_nos[0] == 2 &&
               w.agents[1].m5.attached_bone_nos[0] == 4 &&
               w.agents[1].m5.attached_use_bone_rot[0] &&
               close(w.agents[1].m5.attached_offsets[0].x, 7.0f) &&
               close(w.agents[1].m5.attached_offsets[0].y, 8.0f) &&
               close(w.agents[1].m5.attached_offsets[0].z, 9.0f),
           "attached prop state stored");

    // 1739, 1763, 1764, 1767..1769 animations & action dirs
    w.agents[1].m5.combat_state = 2;
    w.agents[1].m5.attack_action = 1;
    w.agents[1].m5.action_channels[1].action_no = 0x136;  // defend/attack table check
    expect(run_stmt(ctx, 1739U, {reg(0), 1}), "1739 combat state");
    expect(run_stmt(ctx, 1763U, {reg(1), 1}), "1763 attack action");
    expect(run_stmt(ctx, 1764U, {reg(2), 1}), "1764 defend action");
    expect(run_stmt(ctx, 1768U, {reg(3), 1, 1}), "1768 get animation");
    expect(ctx.registers[0] == 2 && ctx.registers[1] == 1 && ctx.registers[2] == 0 &&
               ctx.registers[3] == 0x136,
           "combat/anim getters match");

    w.agents[1].m5.attack_action = 0;
    w.agents[1].m5.defend_action = 1;
    expect(run_stmt(ctx, 1767U, {reg(4), 1}), "1767 defend dir");
    expect(ctx.registers[4] == 5, "1767 defend table dir");
    w.agents[1].m5.action_channels[1].action_no = 0x80;
    expect(run_stmt(ctx, 1769U, {1}), "1769 parried action true");
    w.agents[1].m5.action_channels[1].action_no = 1;
    expect(!run_stmt(ctx, 1769U, {1}), "1769 parried action false");

    // 1777 & 1778 offers, 1803 relations, 1761 enemies following, 1806..1808, 1825, 2076, 2077, 2081..2083, 2670, 2671
    ctx.timersTotal[2] = 1000000LL;  // 10s (1/100,000th-sec ticks)
    expect(run_stmt(ctx, 1777U, {1, 2, 5000}), "1777 add offer");
    expect(run_stmt(ctx, 1778U, {1, 2}), "1778 check offer valid");
    ctx.timersTotal[2] = 2000000LL;  // 20s -> expired
    expect(!run_stmt(ctx, 1778U, {1, 2}), "1778 offer expired");

    expect(run_stmt(ctx, 1803U, {1, 2, -1}), "1803 relation enemy");
    w.agents[2].m5.ai_behavior_target_agent_no = 1;
    w.agents[2].horse_agent_no = 3;
    expect(run_stmt(ctx, 1761U, {reg(0), 1}), "1761 enemies following");
    expect(ctx.registers[0] == 2, "1761 counts enemy rider + horse");

    expect(run_stmt(ctx, 1807U, {1, 1}), "1807 set alarmed");
    expect(run_stmt(ctx, 1806U, {1}), "1806 is alarmed true");
    w.agents[1].m5.sound_channel = 3;
    expect(run_stmt(ctx, 1808U, {1}), "1808 stop sound");
    expect(w.agents[1].m5.sound_channel == -1, "1808 cleared channel");

    w.agents[1].equipped_items[2] = 2;
    w.agents[1].item_ammo[2] = 24;
    expect(run_stmt(ctx, 1825U, {reg(0), 1, 2}), "1825 ammo for slot");
    expect(ctx.registers[0] == 24, "1825 reads slot ammo");

    w.agents[1].position.o = {1.0f, 2.0f, 3.0f, 1.0f};
    w.agents[1].m5.bones.resize(1);
    w.agents[1].m5.bones[0].transform = wse::core::WseTransform::identity();
    w.agents[1].m5.bones[0].transform.o = {0.0f, 1.0f, 0.5f, 1.0f};
    expect(run_stmt(ctx, 2076U, {0, 1, 0, 1}), "2076 bone pos world");
    expect(close(ctx.positions[0].o.x, 1.0f) && close(ctx.positions[0].o.y, 3.0f) &&
               close(ctx.positions[0].o.z, 3.5f),
           "2076 transforms bone to world");

    w.agents[1].m5.ai_move_target_agent_no = 2;
    w.agents[1].m5.ai_behavior_target_agent_no = 0;
    w.agents[1].m5.cached_enemies = {2, 3};
    expect(run_stmt(ctx, 2077U, {1, 1}), "2077 ai interact");
    expect(run_stmt(ctx, 2081U, {reg(0), 1}), "2081 ai move target");
    expect(run_stmt(ctx, 2082U, {reg(1), 1}), "2082 ai behavior target");
    expect(run_stmt(ctx, 2083U, {1, 1}), "2083 ai can crouch");
    expect(run_stmt(ctx, 2670U, {reg(2), 1}), "2670 num cached enemies");
    expect(run_stmt(ctx, 2671U, {reg(3), 1, 1}), "2671 cached enemy 1");
    expect(ctx.registers[0] == 2 && ctx.registers[1] == 0 && ctx.registers[2] == 2 &&
               ctx.registers[3] == 3 && w.agents[1].m5.ai_interact_with_player,
           "AI target/cached enemies getters");

    // 1826 agent_is_in_line_of_sight
    w.fx.ray_obstacles.clear();
    w.fx.ray_obstacles.push_back(
        {true, 0, 0xffffffffu, {4.5f, -5.0f, -5.0f}, {5.5f, 5.0f, 5.0f}});
    w.agents[0].position.o = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx.positions[2] = wse::core::WseTransform::identity();
    ctx.positions[2].o = {10.0f, 0.0f, 1.0f, 1.0f};
    expect(!run_stmt(ctx, 1826U, {0, 2}), "1826 blocked by ray obstacle AABB");
    ctx.positions[2].o = {2.0f, 0.0f, 1.0f, 1.0f};
    expect(run_stmt(ctx, 1826U, {0, 2}), "1826 clear short range");

    // 1827 agent_deliver_damage_to_agent_advanced (friendly fire branch + normal branch)
    w.is_multiplayer = true;
    w.m5.server_friendly_fire = 1;
    w.m5.server_ff_friend_ratio = 50;
    w.m5.server_ff_self_ratio = 25;
    expect(run_stmt(ctx, 1803U, {0, 1, 1}), "1803 friend relation");
    expect(run_stmt(ctx, 1827U, {reg(0), 0, 1, 40, 2}), "1827 MP friendly fire damage");
    expect(ctx.registers[0] == 20 && close(w.agents[1].hit_points, 80.0f) &&
               close(w.agents[0].hit_points, 90.0f),
           "1827 scales friend (50%) and self (25%) dmg");

    w.is_multiplayer = false;
    expect(run_stmt(ctx, 1827U, {reg(0), 0, 2, 30, 2}), "1827 SP direct damage");
    expect(ctx.registers[0] == 30 && w.agents[0].m5.last_inflicted_damage == 30,
           "1827 SP returns inflicted damage");

    // 1973 spawn_horse & 1755 remove_agent & 1760 elapsed
    expect(run_stmt(ctx, 1973U, {3}), "1973 spawn_horse");
    const std::int64_t horse_id = ctx.registers[0];
    expect(horse_id == 4 && w.agents[4].type == 1 && w.agents[4].horse_item_no == 3,
           "1973 spawns new horse agent");

    w.agents[2].rider_agent_no = 1;
    w.agents[1].horse_agent_no = 2;
    ctx.timersTotal[2] = 5000000LL;  // 50s
    expect(run_stmt(ctx, 1755U, {2}), "1755 remove_agent");
    expect(w.agents[2].status == 0 && w.agents[1].horse_agent_no == -1 &&
               w.agents[2].m5.death_timer_active,
           "1755 unlinks rider/horse and starts death timer");
    ctx.timersTotal[2] = 5600000LL;  // 56s -> +6s
    expect(run_stmt(ctx, 1760U, {reg(0), 2}), "1760 time elapsed since removed");
    expect(ctx.registers[0] == 6, "1760 reports 6 seconds");
}

void test_mp_players_m5() {
    wse::vm::VmContext ctx;
    init_m5_world(ctx);
    auto& w = ctx.world;
    w.is_multiplayer = true;
    w.is_server = false;

    // 388..393 client events
    ctx.stringRegisters[1] = u"hello srv";
    expect(run_stmt(ctx, 388U, {10}), "388 send msg");
    expect(run_stmt(ctx, 389U, {11, 7}), "389 send 1 int");
    expect(run_stmt(ctx, 390U, {12, 1, 2}), "390 send 2 int");
    expect(run_stmt(ctx, 391U, {13, 1, 2, 3}), "391 send 3 int");
    expect(run_stmt(ctx, 392U, {14, 1, 2, 3, 4}), "392 send 4 int");
    expect(run_stmt(ctx, 393U, {15, 1}), "393 send string");
    expect(w.m5.client_events.size() == 6 && w.m5.client_events[4].num_ints == 4 &&
               w.m5.client_events[5].text == u"hello srv",
           "388..393 queued client events");

    w.is_server = true;
    // 409, 410, 425, 429, 431..442
    expect(run_stmt(ctx, 409U, {1, 9}), "409 player_spawn_new_agent");
    expect(run_stmt(ctx, 410U, {1, 2, 3}), "410 player_add_spawn_item");
    expect(run_stmt(ctx, 425U, {reg(0), 0, 0}), "425 find spawn point");
    expect(ctx.registers[0] >= 0 && ctx.registers[0] < 64, "425 returned valid entry point");
    expect(run_stmt(ctx, 429U, {1, 1}), "429 player_set_is_admin");
    expect(w.players[1].is_admin && w.players[1].privilege == 3, "429 sets privilege=3");

    expect(run_stmt(ctx, 431U, {reg(0), 1}), "431 player_get_score");
    expect(run_stmt(ctx, 433U, {reg(1), 1}), "433 player_get_kill_count");
    expect(run_stmt(ctx, 435U, {reg(2), 1}), "435 player_get_death_count");
    expect(run_stmt(ctx, 437U, {reg(3), 1}), "437 player_get_ping");
    expect(run_stmt(ctx, 441U, {reg(4), 1}), "441 player_get_unique_id");
    expect(run_stmt(ctx, 442U, {reg(5), 1}), "442 player_get_gender");
    expect(ctx.registers[0] == 42 && ctx.registers[1] == 7 && ctx.registers[2] == 2 &&
               ctx.registers[3] == 35 && ctx.registers[4] == 0x543210 &&
               ctx.registers[5] == 1,
           "player getters + 24-bit unique_id mask");

    expect(run_stmt(ctx, 434U, {1, 99}), "434 set kill count");
    expect(run_stmt(ctx, 436U, {1, 11}), "436 set death count");
    w.players[1].m5.is_busy_with_menus = true;
    expect(run_stmt(ctx, 438U, {1}), "438 busy with menus true");
    expect(run_stmt(ctx, 440U, {1, 1, 1}), "440 mute globally");
    expect(run_stmt(ctx, 439U, {reg(6), 1}), "439 get muted");
    expect(w.players[1].kill_count == 99 && w.players[1].death_count == 11 &&
               w.players[1].is_muted && w.players[1].m5.is_muted_globally &&
               ctx.registers[6] == 1,
           "player setters");

    // 459, 460
    w.agents[0].equipped_items[0] = 2;
    w.agents[0].equipped_items[1] = 1;
    w.agents[0].horse_item_no = 3;
    w.agents[0].horse_agent_no = 3;
    w.agents[3].rider_agent_no = 0;
    w.agents[3].horse_item_no = 3;
    expect(run_stmt(ctx, 459U, {0}), "459 save picked up items");
    expect(w.players[0].m5.spawn_items_saved && w.players[0].item_ids[0] == 2 &&
               w.players[0].item_ids[8] == 3,
           "459 saves weapon + horse");
    expect(run_stmt(ctx, 460U, {reg(0), 0}), "460 value of original items");
    expect(ctx.registers[0] == (500 + 250 + 900), "460 sums items + horse value");

    // 475..499 server config
    w.m5.allow_renaming_server = true;
    w.m5.allow_changing_game_type = true;
    expect(run_stmt(ctx, 475U, {reg(0)}), "475 allow renaming");
    expect(run_stmt(ctx, 476U, {reg(1)}), "476 allow game type");
    expect(ctx.registers[0] == 1 && ctx.registers[1] == 1, "475/476");
    expect(run_stmt(ctx, 479U, {3}), "479 combat speed");
    expect(run_stmt(ctx, 481U, {1}), "481 friendly fire");
    expect(run_stmt(ctx, 483U, {1}), "483 block dir");
    ctx.stringRegisters[0] = u"Secret_123";
    expect(run_stmt(ctx, 484U, {0}), "484 valid server password");
    expect(run_stmt(ctx, 486U, {1}), "486 add to servers list");
    expect(run_stmt(ctx, 488U, {2}), "488 ghost mode");
    ctx.stringRegisters[0] = u"EU_Arena_1";
    expect(run_stmt(ctx, 489U, {0}), "489 valid server name");
    expect(run_stmt(ctx, 491U, {32, 64}), "491 max num players");
    ctx.stringRegisters[0] = u"Welcome!";
    expect(run_stmt(ctx, 492U, {0}), "492 welcome message");
    expect(run_stmt(ctx, 494U, {1}), "494 melee ff");
    expect(run_stmt(ctx, 496U, {25}), "496 ff self ratio");
    expect(run_stmt(ctx, 498U, {50}), "498 ff friend ratio");
    expect(run_stmt(ctx, 499U, {reg(2)}), "499 anti cheat");

    expect(run_stmt(ctx, 478U, {reg(3)}), "478 get combat speed");
    expect(run_stmt(ctx, 480U, {reg(4)}), "480 get friendly fire");
    expect(run_stmt(ctx, 482U, {reg(5)}), "482 get block dir");
    expect(run_stmt(ctx, 485U, {reg(6)}), "485 get servers list");
    expect(run_stmt(ctx, 487U, {reg(7)}), "487 get ghost mode");
    expect(run_stmt(ctx, 490U, {reg(8)}), "490 get max players");
    expect(run_stmt(ctx, 493U, {reg(9)}), "493 get melee ff");
    expect(run_stmt(ctx, 495U, {reg(10)}), "495 get ff self");
    expect(run_stmt(ctx, 497U, {reg(11)}), "497 get ff friend");

    expect(w.mp.server_password == "Secret_123" &&
               w.mp.server_name == "EU_Arena_1" &&
               w.mp.welcome_message == "Welcome!" &&
               w.m5.max_private_players == 64 &&
               ctx.registers[2] == 0 && ctx.registers[3] == 3 &&
               ctx.registers[4] == 1 && ctx.registers[5] == 1 &&
               ctx.registers[6] == 1 && ctx.registers[7] == 2 &&
               ctx.registers[8] == 32 && ctx.registers[9] == 1 &&
               ctx.registers[10] == 25 && ctx.registers[11] == 50,
           "server getters/setters round-trip");

    // 2748 player_set_face_keys
    ctx.stringRegisters[2] =
        u"000000001234567800000000abcdef0100000000000000000000000000000000";
    expect(run_stmt(ctx, 2748U, {1, 2}), "2748 player_set_face_keys");
    expect(w.players[1].face_keys.keys[0] == 0x12345678ULL &&
               w.players[1].face_keys.keys[1] == 0xabcdef01ULL,
           "2748 parsed 64-hex face keys");
}

void test_scenes_props_m5() {
    wse::vm::VmContext ctx;
    init_m5_world(ctx);
    auto& w = ctx.world;

    // 1781, 1782 entry points
    ctx.positions[0] = wse::core::WseTransform::identity();
    ctx.positions[0].o = {1.0f, 2.0f, -150.0f, 1.0f};
    expect(run_stmt(ctx, 1781U, {5, 0}), "1781 entry_point_set_position");
    expect(w.entry_points[5].valid && close(w.entry_points[5].position.o.z, 0.0f),
           "1781 clamps z < -100 to 0");
    w.entry_points[5].is_auto_generated = true;
    expect(run_stmt(ctx, 1782U, {5}), "1782 entry_point_is_auto_generated");

    // 1797, 1798 weather & fog
    w.lighting.skybox_no = 2;
    expect(run_stmt(ctx, 1797U, {1, 80}), "1797 set_rain");
    expect(run_stmt(ctx, 1798U, {120, 0x112233}), "1798 set_fog_distance");
    expect(w.m5.rain_type == 1 && close(w.m5.rain_strength, 0.8f) &&
               w.m5.precipitation_type == 3 &&
               close(w.m5.fog_distance, 120.0f) &&
               w.m5.skybox_fog_colors[2] == 0x112233U,
           "rain/fog updated");

    // 1801, 1816, 1819, 1822, 1823, 1830..1834, 1838..1841
    w.agents[1].m5.standing_mission_object_no = 1;
    expect(run_stmt(ctx, 1801U, {1, 1}), "1801 has agent on it true");
    expect(run_stmt(ctx, 1816U, {reg(0), 1}), "1816 max hit points");
    expect(ctx.registers[0] == 150, "1816 returns 150");
    w.mission_objects[1].m5.enable_pruning = true;
    expect(run_stmt(ctx, 1819U, {1, 30}), "1819 set_prune_time");
    expect(run_stmt(ctx, 1822U, {1, 2000}), "1822 fade_out");
    expect(w.mission_objects[1].m5.fade_out &&
               close(w.mission_objects[1].m5.fade_duration, 2.0f) &&
               close(w.mission_objects[1].m5.prune_time, 30.0f),
           "1819/1822 updated");
    expect(run_stmt(ctx, 1823U, {1, 500}), "1823 fade_in");
    expect(!w.mission_objects[1].m5.fade_out &&
               close(w.mission_objects[1].m5.fade_duration, 0.5f),
           "1823 updated");

    w.mission_objects[2].is_item = true;
    w.mission_objects[2].m5.is_spawned_item = false;
    w.mission_objects[2].scene_prop_no = 1;
    w.mission_objects[3].is_item = true;
    w.mission_objects[3].m5.is_spawned_item = true;
    w.mission_objects[3].scene_prop_no = 1;
    expect(run_stmt(ctx, 1830U, {reg(0), 1}), "1830 scene item count");
    expect(run_stmt(ctx, 1831U, {reg(1), 1, 0}), "1831 scene item instance");
    expect(run_stmt(ctx, 1832U, {reg(2), 1}), "1832 spawned item count");
    expect(run_stmt(ctx, 1833U, {reg(3), 1, 0}), "1833 spawned item instance");
    expect(!run_stmt(ctx, 1833U, {reg(4), 1, 1}), "1833 missing ordinal fails");
    expect(ctx.registers[0] == 1 && ctx.registers[1] == 2 &&
               ctx.registers[2] == 1 && ctx.registers[3] == 3,
           "scene/spawned item instances match");

    w.mission_objects[0].variation_id_no = 11;
    w.mission_objects[0].variation_id_2_no = 22;
    expect(run_stmt(ctx, 1838U, {0}), "1838 valid");
    expect(run_stmt(ctx, 1840U, {reg(0), 0}), "1840 variation 1");
    expect(run_stmt(ctx, 1841U, {reg(1), 0}), "1841 variation 2");
    expect(ctx.registers[0] == 11 && ctx.registers[1] == 22, "variations match");

    // 1865..1887
    ctx.positions[1] = wse::core::WseTransform::identity();
    ctx.positions[1].o = {4.0f, 5.0f, 6.0f, 1.0f};
    expect(run_stmt(ctx, 1865U, {0, 1, 250, 90}), "1865 rotate to pos");
    expect(w.mission_objects[0].m5.is_rotating &&
               close(w.mission_objects[0].animate_duration, 2.5f),
           "1865 rotation state");
    expect(run_stmt(ctx, 1866U, {0}), "1866 init rotation angles");
    expect(run_stmt(ctx, 1870U, {0}), "1870 refill hit points");
    expect(close(w.mission_objects[0].hit_points, 150.0f), "1870 refilled");
    expect(run_stmt(ctx, 1871U, {0, 1}), "1871 dynamics properties");
    expect(run_stmt(ctx, 1872U, {0, 1}), "1872 velocity");
    expect(run_stmt(ctx, 1873U, {0, 1}), "1873 omega");
    expect(run_stmt(ctx, 1874U, {0, 1}), "1874 impulse");
    expect(run_stmt(ctx, 1877U, {0, -1, 200}), "1877 receive damage");
    expect(!w.mission_objects[0].m5.collision_enabled &&
               w.mission_objects[0].hit_points <= 0.0f,
           "1877 disables collision on destruction");

    w.mission_objects[0].m5.collision_enabled = true;
    w.mission_objects[1].m5.collision_enabled = true;
    w.mission_objects[0].position.o = {0.0f, 0.0f, 0.0f, 1.0f};
    w.mission_objects[1].position.o = {1.5f, 0.0f, 0.0f, 1.0f};
    expect(run_stmt(ctx, 1880U, {0, 1}), "1880 intersects true");
    w.mission_objects[1].position.o = {5.0f, 0.0f, 0.0f, 1.0f};
    expect(!run_stmt(ctx, 1880U, {0, 1}), "1880 intersects false");

    expect(run_stmt(ctx, 1881U, {0, 3, 1}), "1881 play sound");
    expect(w.mission_objects[0].m5.sound_playing && w.played_sounds.size() == 1,
           "1881 recorded sound");
    expect(run_stmt(ctx, 1882U, {0}), "1882 stop sound");
    expect(!w.mission_objects[0].m5.sound_playing, "1882 stopped sound");

    expect(run_stmt(ctx, 1886U, {0, 2, 1}), "1886 add particle sys");
    expect(w.mission_objects[0].m5.particle_system_nos.size() == 1, "1886 pushed");
    expect(run_stmt(ctx, 1887U, {0}), "1887 stop particles");
    expect(w.mission_objects[0].m5.particle_system_nos.empty(), "1887 cleared");

    // 1889 replace_prop_instance: gated on caller == -20 || -19
    expect(run_stmt(ctx, 1889U, {0, 4}, 0), "1889 non-init caller no-op");
    expect(w.mission_objects[0].scene_prop_no == 1, "1889 blocked for caller 0");
    expect(run_stmt(ctx, 1889U, {0, 4}, -19), "1889 init caller succeeds");
    expect(w.mission_objects[0].scene_prop_no == 4, "1889 replaced scene_prop_no");

    // 1974 spawn_scene_prop
    w.spawn_position = ctx.positions[1];
    expect(run_stmt(ctx, 1974U, {2, 3}), "1974 spawn_scene_prop");
    expect(ctx.registers[0] == 4 && w.mission_objects[4].scene_prop_no == 2 &&
               w.mission_objects[4].variation_id_no == 3,
           "1974 spawned new mission object");

    // 2001, 2008..2019 mission_cam_*
    expect(run_stmt(ctx, 2001U, {1, 0}), "2001 cam mode");
    expect(run_stmt(ctx, 2008U, {0xff00ff}), "2008 cam screen color");
    expect(run_stmt(ctx, 2009U, {0x00ff00, 1500}), "2009 animate screen color");
    expect(run_stmt(ctx, 2011U, {1}), "2011 cam set position");
    expect(run_stmt(ctx, 2010U, {2}), "2010 cam get position");
    expect(close(ctx.positions[2].o.x, 4.0f), "2010 matches 2011");
    expect(run_stmt(ctx, 2014U, {75}), "2014 cam set aperture");
    expect(run_stmt(ctx, 2013U, {reg(0)}), "2013 cam get aperture");
    expect(ctx.registers[0] == 75, "2013 matches 2014");
    expect(run_stmt(ctx, 2015U, {90, 500, 1}), "2015 cam animate aperture");
    expect(run_stmt(ctx, 2016U, {1, 80, 1000, 1}), "2016 cam anim pos+ap");
    expect(run_stmt(ctx, 2017U, {1, 1}), "2017 target agent");
    expect(run_stmt(ctx, 2019U, {12}), "2019 cam animation");
    expect(w.m5.mission_camera.target_agent_no == 1 &&
               w.m5.mission_camera.target_agent_use_rotation &&
               w.m5.mission_camera.animation_no == 12 &&
               w.m5.mission_camera.non_linear,
           "camera state stored");
    expect(run_stmt(ctx, 2018U, {}), "2018 clear target agent");
    expect(w.m5.mission_camera.target_agent_no == -1, "2018 cleared");

    // 2400..2403 shader params & 2610..2617 deform/material
    ctx.stringRegisters[0] = u"uTint";
    expect(run_stmt(ctx, 2400U, {0, 7}), "2400 shader int");
    expect(run_stmt(ctx, 2401U, {0, 2500}), "2401 shader float overwrites");
    expect(w.m5.shader_params.size() == 1 &&
               close(w.m5.shader_params[0].second.float_val, 2.5f),
           "2401 upserted shader float");
    ctx.stringRegisters[1] = u"uMatrix";
    expect(run_stmt(ctx, 2403U, {1, 1000, 0, 0, 0, 1000, 0, 0, 0, 1000, 0, 0, 0}),
           "2403 shader float4x4");
    expect(w.m5.shader_params.size() == 2 &&
               close(w.m5.shader_params[1].second.matrix_val[0], 1.0f) &&
               close(w.m5.shader_params[1].second.matrix_val[15], 1.0f),
           "2403 mat4 stored");

    expect(run_stmt(ctx, 2611U, {0, 5, 25, 2000}), "2611 deform in range");
    expect(run_stmt(ctx, 2616U, {reg(0), 0}), "2616 get deform frame");
    w.mission_objects[0].m5.deform_progress = 0.75f;
    expect(run_stmt(ctx, 2615U, {reg(1), 0}), "2615 get deform progress");
    ctx.stringRegisters[2] = u"mat brick wall";
    expect(run_stmt(ctx, 2617U, {0, 2, 2}), "2617 set material");
    expect(ctx.registers[0] == 5 && ctx.registers[1] == 75 &&
               w.mission_objects[0].m5.material_submesh == 2 &&
               w.mission_objects[0].m5.material_name == "mat_brick_wall",
           "deform & underscored material stored");
}

void test_ui_menus_m5() {
    wse::vm::VmContext ctx;
    init_m5_world(ctx);
    auto& w = ctx.world;

    // 75, 750, 751 cursor & Warband rglTransform camera (s=right, f=up, -u=forward)
    w.m5.cursor_pos = {0.5f, 0.375f};
    w.misc.camera_transform.s = {1.0f, 0.0f, 0.0f, 0.0f};
    w.misc.camera_transform.f = {0.0f, 0.0f, 1.0f, 0.0f};
    w.misc.camera_transform.u = {0.0f, -1.0f, 0.0f, 0.0f};
    w.misc.camera_transform.o = {0.0f, 0.0f, 0.0f, 0.0f};
    w.misc.camera_fov_deg = 60.0f;
    w.misc.viewport_width = 1600.0f;
    w.misc.viewport_height = 900.0f;
    expect(run_stmt(ctx, 75U, {0}), "75 mouse_get_position");
    expect(close(ctx.positions[0].o.x, 0.5f) && close(ctx.positions[0].o.y, 0.375f),
           "75 cursor position");
    expect(run_stmt(ctx, 751U, {1, 2}), "751 mouse_get_world_projection");
    expect(close(ctx.positions[1].o.x, 0.0f) && close(ctx.positions[2].o.y, 1000.0f),
           "751 center ray shoots along camera forward -u = +Y");

    // 750 <-> 751 exact round-trip in Warband UI coords ([0,1] x [0,0.75])
    ctx.positions[3].o = {10.0f, 50.0f, 5.0f, 0.0f};
    expect(run_stmt(ctx, 750U, {4, 3}), "750 position_get_screen_projection");
    expect(ctx.positions[4].o.x > 0.5f && ctx.positions[4].o.x < 1.0f &&
               ctx.positions[4].o.y > 0.375f && ctx.positions[4].o.y < 0.75f,
           "750 outputs Warband UI [0,1]x[0,0.75] coords (top-right > center)");
    w.m5.cursor_pos = {ctx.positions[4].o.x, ctx.positions[4].o.y};
    expect(run_stmt(ctx, 751U, {5, 6}), "751 unprojects 750 output");
    const float ray_dx = ctx.positions[6].o.x - ctx.positions[5].o.x;
    const float ray_dy = ctx.positions[6].o.y - ctx.positions[5].o.y;
    const float ray_dz = ctx.positions[6].o.z - ctx.positions[5].o.z;
    const float t_y50 = 50.0f / ray_dy;
    expect(close(ctx.positions[5].o.x + ray_dx * t_y50, 10.0f, 1e-3f) &&
               close(ctx.positions[5].o.z + ray_dz * t_y50, 5.0f, 1e-3f),
           "751 unprojected ray passes through original 750 world point");

    // Same round-trip with an unknown (0x0) viewport: both ops fall back to 4:3.
    w.misc.viewport_width = 0.0f;
    w.misc.viewport_height = 0.0f;
    expect(run_stmt(ctx, 750U, {4, 3}), "750 with unknown viewport");
    w.m5.cursor_pos = {ctx.positions[4].o.x, ctx.positions[4].o.y};
    expect(run_stmt(ctx, 751U, {5, 6}), "751 with unknown viewport");
    {
        const float dx = ctx.positions[6].o.x - ctx.positions[5].o.x;
        const float dy = ctx.positions[6].o.y - ctx.positions[5].o.y;
        const float dz = ctx.positions[6].o.z - ctx.positions[5].o.z;
        const float t = 50.0f / dy;
        expect(close(ctx.positions[5].o.x + dx * t, 10.0f, 1e-3f) &&
                   close(ctx.positions[5].o.z + dz * t, 5.0f, 1e-3f),
               "750/751 round-trip with an unknown viewport (shared 4:3 fallback)");
    }
    w.misc.viewport_width = 1600.0f;
    w.misc.viewport_height = 900.0f;

    // 911..956 presentation overlays
    w.active_presentation_no = 1;
    ctx.stringRegisters[0] = u"Click Me";
    ctx.stringRegisters[1] = u"mat ui btn";
    ctx.stringRegisters[2] = u"Tooltip help";
    expect(run_stmt(ctx, 911U, {reg(0), 1}), "911 create_mesh_overlay");
    expect(run_stmt(ctx, 912U, {reg(1), 0, 0x0800}), "912 create_button_overlay");
    expect(run_stmt(ctx, 913U, {reg(2), 1, 2}), "913 create_image_button_overlay");
    expect(run_stmt(ctx, 914U, {reg(3), 0, 100}), "914 create_slider_overlay");
    expect(run_stmt(ctx, 915U, {reg(4), 0, 50}), "915 create_progress_overlay");
    expect(run_stmt(ctx, 916U, {reg(5)}), "916 create_combo_button_overlay");
    expect(run_stmt(ctx, 917U, {reg(6)}), "917 create_text_box_overlay");
    expect(run_stmt(ctx, 918U, {reg(7), 1, 2}), "918 create_check_box_overlay");
    expect(run_stmt(ctx, 938U, {reg(8), 1, 2, 77}), "938 image button tableau");
    expect(run_stmt(ctx, 944U, {reg(9), 2}), "944 mesh with item id");
    expect(w.overlays.size() == 10 && w.overlays[1].text == u"Click Me" &&
               w.overlays[3].m5.max_value == 100.0 && w.overlays[7].m5.is_checkbox &&
               w.overlays[8].m5.tableau_material_no == 2 && w.overlays[9].m5.item_no == 2,
           "911..944 overlays created");

    expect(run_stmt(ctx, 945U, {0}), "945 set_container_overlay");
    expect(run_stmt(ctx, 919U, {reg(10)}), "919 create_simple_text_box inside container");
    expect(run_stmt(ctx, 949U, {ctx.registers[10]}), "949 obtain focus");
    expect(run_stmt(ctx, 950U, {ctx.registers[10], 2}), "950 set tooltip");
    expect(run_stmt(ctx, 956U, {0, 1}), "956 set material");
    w.overlays[0].position = {12.0f, 34.0f};
    expect(run_stmt(ctx, 946U, {3, 0}), "946 overlay_get_position");
    expect(w.overlays[10].m5.container_overlay_no == 0 && w.overlays[10].m5.focused &&
               w.m5.focused_overlay_no == 10 &&
               w.overlays[10].m5.tooltip == u"Tooltip help" &&
               w.overlays[0].m5.material_name == "mat_ui_btn" &&
               close(ctx.positions[3].o.x, 12.0f) && close(ctx.positions[3].o.y, 34.0f),
           "overlay container/focus/tooltip/material/pos");

    // 1120..1126 tutorial/question dialogs & tutorial_message
    ctx.stringRegisters[0] = u"Prompt Body";
    ctx.stringRegisters[1] = u"Custom Title";
    expect(run_stmt(ctx, 1120U, {0, 1}), "1120 tutorial_box");
    expect(w.m5.yes_no_window_open && !w.m5.yes_no_is_question &&
               w.m5.dialog_title == u"Custom Title",
           "1120 sets tutorial modal");
    w.m5.yes_no_window_open = false;
    w.translations["ui_yes"] = "Confirm";
    expect(run_stmt(ctx, 1121U, {0}), "1121 question_box translation fallback");
    expect(w.m5.yes_no_window_open && w.m5.yes_no_is_question &&
               w.m5.dialog_yes_text == u"Confirm" && w.m5.dialog_no_text == u"No",
           "1121 translates ui_yes and defaults ui_no");

    expect(run_stmt(ctx, 1122U, {0, 0xff112233, 4}), "1122 tutorial_message");
    expect(run_stmt(ctx, 1123U, {100, 200}), "1123 tutorial_message_set_position");
    expect(run_stmt(ctx, 1124U, {500, 300}), "1124 tutorial_message_set_size");
    expect(run_stmt(ctx, 1125U, {1}), "1125 center_justify");
    expect(run_stmt(ctx, 1126U, {1}), "1126 background");
    expect(w.m5.message_box_text == u"Prompt Body" &&
               w.m5.message_box_color == 0xff112233U &&
               close(w.m5.message_box_position.x, 0.1f) &&
               close(w.m5.message_box_size.y, 0.3f) &&
               w.m5.message_box_center_justify &&
               w.m5.message_box_bg_mesh == "white_plane",
           "1122..1126 message box state");
    expect(run_stmt(ctx, 1122U, {-1}), "1122 clear message");
    expect(w.m5.message_box_text.empty(), "1122 -1 clears text");

    // 2040..2058 screen transitions
    expect(run_stmt(ctx, 2040U, {}), "2040 change_screen_return");
    expect(run_stmt(ctx, 2041U, {0}), "2041 change_screen_loot");
    expect(run_stmt(ctx, 2043U, {1, 2}), "2043 change_screen_exchange_members");
    expect(run_stmt(ctx, 2053U, {5, 1}), "2053 change_screen_notes info_page");
    expect(run_stmt(ctx, 2057U, {}), "2057 change_screen_controls");
    expect(run_stmt(ctx, 2058U, {}), "2058 change_screen_options");
    expect(w.m5.enqueued_windows.size() == 6 &&
               w.m5.enqueued_windows[0].first == 1 &&
               w.m5.enqueued_windows[1].first == 2 &&
               w.m5.enqueued_windows[2].first == 6 &&
               w.m5.enqueued_windows[3].first == 0x10 &&
               w.m5.enqueued_windows[4].first == 0x13 &&
               w.m5.enqueued_windows[5].first == 0x14,
           "2040..2058 window IDs queued");
    expect(run_stmt(ctx, 2055U, {}), "2055 change_screen_quit");
    expect(w.m5.quit_requested && w.m5.enqueued_windows.empty() &&
               w.m5.loading_mode == 5,
           "2055 quit clears queue and sets flags");

    // INT64_MIN boundary inputs across M5 groups
    const std::int64_t imin = std::numeric_limits<std::int64_t>::min();
    expect(run_stmt(ctx, 1689U, {imin, imin}), "1689 INT64_MIN safe");
    expect(run_stmt(ctx, 431U, {reg(0), imin}), "431 INT64_MIN safe");
    expect(run_stmt(ctx, 1819U, {imin, imin}), "1819 INT64_MIN safe");
    expect(run_stmt(ctx, 923U, {imin, imin}), "923 INT64_MIN safe");
}

// Regressions found in the Milestone 5 review.
void test_m5_review_regressions() {
    using wse::vm::VmContext;

    // 1974 spawn_scene_prop spawns at the set_spawn_position (1970) transform.
    {
        VmContext ctx;
        ctx.world.misc.num_scene_props = 5;
        ctx.positions[1].o = {10.0f, 20.0f, 30.0f, 1.0f};
        ctx.world.fx.trigger_transform.o = {-1.0f, -1.0f, -1.0f, 1.0f};
        expect(run_stmt(ctx, 1970U, {1}), "1970 set_spawn_position");
        expect(run_stmt(ctx, 1974U, {2}), "1974 spawn_scene_prop");
        const auto& o = ctx.world.mission_objects[0].position.o;
        expect(ctx.registers[0] == 0 && close(o.x, 10.0f) && close(o.y, 20.0f) &&
                   close(o.z, 30.0f),
               "1974 uses the spawn position, not the trigger transform");

        // 1974 is capped like spawn_item.
        ctx.world.mission_objects.resize(wse::game::kMaxSpawnedMissionObjects);
        ctx.registers[0] = -7;
        expect(run_stmt(ctx, 1974U, {2}), "1974 at cap");
        expect(ctx.world.mission_objects.size() ==
                       wse::game::kMaxSpawnedMissionObjects &&
                   ctx.registers[0] == -7,
               "1974 does not grow past kMaxSpawnedMissionObjects");
    }

    {
        VmContext ctx;
        auto& w = ctx.world;
        w.items.resize(2);
        w.agents.resize(2);
        for (auto& ag : w.agents) {
            ag.active = true;
            ag.status = 1;
            ag.hit_points = 100.0f;
            ag.max_hit_points = 100.0f;
            ag.damage_modifier = 1.0f;
        }

        // 1748 must not write the ground height back into the script register.
        w.ground_level_z = 5.0f;
        ctx.positions[3].o = {1.0f, 2.0f, 99.0f, 1.0f};
        expect(run_stmt(ctx, 1748U, {0, 3, 1}), "1748 no-attack destination");
        expect(close(ctx.positions[3].o.z, 99.0f) &&
                   close(w.agents[0].target_position.o.z, 5.0f),
               "1748 clamps the agent target, not the position register");

        // 1827 with weapon -1 still applies the explicit damage.
        ctx.registers[9] = -555;
        expect(run_stmt(ctx, 1827U, {reg(9), 0, 1, 40, -1}), "1827 item -1");
        expect(close(w.agents[1].hit_points, 60.0f) && ctx.registers[9] == 40,
               "1827 delivers explicit damage without a weapon item");
        ctx.registers[9] = -555;
        expect(run_stmt(ctx, 1827U, {reg(9), 0, 1, 15}), "1827 item omitted");
        expect(close(w.agents[1].hit_points, 45.0f) && ctx.registers[9] == 15,
               "1827 delivers damage when the weapon operand is omitted");

        // 1973 spawn_horse is capped.
        w.agents.resize(wse::game::kMaxSpawnedAgents);
        expect(run_stmt(ctx, 1973U, {0}), "1973 at cap");
        expect(w.agents.size() == wse::game::kMaxSpawnedAgents &&
                   ctx.registers[0] == -1,
               "1973 does not grow past kMaxSpawnedAgents");
    }

    // 928 overlay_set_boundaries saturates out-of-range bounds.
    {
        VmContext ctx;
        ctx.fixedPointMultiplier = 1;
        ctx.world.overlays.resize(1);
        ctx.world.overlays[0].type = 2;
        ctx.world.overlays[0].val = 7;
        expect(run_stmt(ctx, 928U,
                        {0, 4000000000000000LL, 8000000000000000LL}),
               "928 huge bounds");
        expect(ctx.world.overlays[0].val ==
                   std::numeric_limits<std::int32_t>::max(),
               "928 clamps val to a saturated lower bound");
    }

    // Phase 2E mission containers (`rglStableVector<mbAgent>` & `rglStableVector<mbMissionObject>`
    // LIFO recycling, tail shrink, referential unlinking, 30s fade-out, item pruning, and 416 clear).
    {
        VmContext ctx;
        auto& w = ctx.world;
        w.lighting.has_scene = true;
        w.is_multiplayer = true;
        w.misc.num_scene_props = 4;
        w.items.resize(2);
        w.items[0].hit_points = 100;
        w.troops.resize(1);
        w.init_players(4);
        expect(w.connect_player(1, "Knight", 777, 1), "connect player 1");

        expect(run_stmt(ctx, 1972U, {0}), "1972 spawn agent 0");
        expect(run_stmt(ctx, 1973U, {0}), "1973 spawn horse 1");
        expect(run_stmt(ctx, 1972U, {0}), "1972 spawn agent 2");
        expect(w.agents.size() == 3U, "3 active agents spawned");

        w.agents[1].rider_agent_no = 0;
        w.agents[0].horse_agent_no = 1;
        w.players[1].agent_no = 1;
        w.agents[1].player_no = 1;
        w.m6.agent(1).fade_out_duration = 5.0f;

        expect(w.remove_agent(1) && !w.is_valid_agent(1) &&
                   w.agents[0].horse_agent_no == -1 &&
                   w.players[1].agent_no == -1 &&
                   w.agents.size() == 3U && w.agent_free_slots.size() == 1U,
               "remove_agent unlinks horse/rider/player and records interior hole");

        expect(run_stmt(ctx, 1972U, {0}) && ctx.registers[0] == 1 &&
                   w.agents.size() == 3U && w.m6.agents.find(1) == w.m6.agents.end(),
               "spawn_agent recycles interior hole 1 LIFO and clears stale M6 state");

        expect(w.remove_agent(2) && w.agents.size() == 2U,
               "removing trailing agent shrinks stable vector back to high-water mark");

        ctx.timersTotal[2] = 1000000LL;  // 10s
        expect(run_stmt(ctx, 1749U, {1}), "1749 agent_fade_out records exit_timer_start");
        ctx.timersTotal[2] = 4000000LL;  // +30s
        w.advance_mission_lifetimes(ctx.timersTotal, 0.1f);
        expect(!w.is_valid_agent(1) && w.agents.size() == 1U,
               "advance_mission_lifetimes removes faded agent after 30.0s and trims tail");

        expect(run_stmt(ctx, 1974U, {1, 0}) && ctx.registers[0] == 0, "1974 prop 0");
        expect(run_stmt(ctx, 1971U, {0, 0, 5}) && ctx.registers[0] == 1, "1971 item 1 (prune=5s)");
        expect(run_stmt(ctx, 1974U, {1, 0}) && ctx.registers[0] == 2, "1974 prop 2");
        w.fx.spawn_effector_mission_object_nos[0] = 1;
        w.m6.mission_object(1).sound_progress = 0.75f;
        w.advance_mission_lifetimes(ctx.timersTotal, 6.0f);
        expect(!w.is_valid_mission_object(1) &&
                   w.fx.spawn_effector_mission_object_nos[0] == -1 &&
                   w.mission_object_free_slots.size() == 1U,
               "advance_mission_lifetimes expires pruned item 1 and unlinks effector");
        expect(run_stmt(ctx, 1974U, {1, 0}) && ctx.registers[0] == 1 &&
                   w.mission_objects.size() == 3U &&
                   w.m6.mission_objects.find(1) == w.m6.mission_objects.end(),
               "spawn_scene_prop recycles freed mission_object hole 1 LIFO");

        expect(run_stmt(ctx, 416U, {}), "416 multiplayer_clear_scene");
        expect(w.agents.empty() && w.mission_objects.size() == 3U &&
                   w.agent_free_slots.empty() && w.mission_object_free_slots.empty() &&
                   w.players[1].agent_no == -1 && !w.players[1].m5.ready_to_spawn,
               "416 clear_scene resets agents/players while keeping non-pruned 1974 props");
    }

    // Review regressions for e6c4d0e (mission containers).
    {
        // A faded-out prop stays alive: fade_in can bring it back and its id is not recycled.
        VmContext ctx;
        auto& w = ctx.world;
        ctx.fixedPointMultiplier = 1;
        w.lighting.has_scene = true;
        w.misc.num_scene_props = 4;
        expect(run_stmt(ctx, 1974U, {1, 0}) && ctx.registers[0] == 0, "fade: spawn prop 0");
        expect(run_stmt(ctx, 1974U, {2, 0}) && ctx.registers[0] == 1, "fade: spawn prop 1");
        expect(run_stmt(ctx, 1822U, {0, 1}), "fade: 1822 scene_prop_fade_out over 1 s");
        w.step_world_frame(ctx.timersTotal, 2.0f);
        expect(w.is_valid_mission_object(0) && w.mission_object_free_slots.empty(),
               "faded-out prop is not removed by the lifetime pass");
        expect(run_stmt(ctx, 1823U, {0, 1}) && !w.mission_objects[0].m5.fade_out,
               "1823 scene_prop_fade_in restores the faded-out prop");
        expect(run_stmt(ctx, 1974U, {3, 0}) && ctx.registers[0] == 2,
               "next spawn gets a fresh id instead of the faded prop's id");
    }
    {
        // Every agent reference is cleared before a slot is recycled.
        VmContext ctx;
        auto& w = ctx.world;
        w.lighting.has_scene = true;
        w.troops.resize(1);
        w.items.resize(1);
        w.init_players(2);
        for (int i = 0; i < 3; ++i) {
            expect(run_stmt(ctx, 1972U, {0}) && ctx.registers[0] == i, "refs: spawn agent");
        }
        w.teams[0].leader_agent_no = 0;
        w.teams[0].order_listener = 0;
        w.teams[0].division_leaders[3] = 0;
        w.agents[1].m5.look_target_agent_no = 0;
        w.agents[1].m5.ai_move_target_agent_no = 0;
        w.agents[1].m5.ai_behavior_target_agent_no = 0;
        w.agents[1].m5.cached_enemies = {2, 0, 2};
        w.agents[1].m5.add_offer(0, 5.0f, ctx.timersTotal);
        w.agents[2].horse_agent_no = 0;  // one-sided link: agent 0 has no rider_agent_no
        w.m5.mission_camera.target_agent_no = 0;
        w.dialog.speaker_agent_no = 0;
        w.dialog.pending_mission_agent_no = 0;
        w.player_agent_no = 0;
        w.players[1].agent_no = 0;
        w.misc.num_scene_props = 2;
        expect(run_stmt(ctx, 1974U, {1, 0}), "refs: spawn prop");
        w.mission_objects[0].parent_agent_no = 0;

        expect(w.remove_agent(0), "refs: remove agent 0");
        const auto& tm = w.teams[0];
        const auto& a1 = w.agents[1].m5;
        expect(tm.leader_agent_no == -1 && tm.order_listener == -1 &&
                   tm.division_leaders[3] == -1 && a1.look_target_agent_no == -1 &&
                   a1.ai_move_target_agent_no == -1 && a1.ai_behavior_target_agent_no == -1 &&
                   a1.cached_enemies == std::vector<std::int32_t>{2, 2} && a1.offers.empty() &&
                   w.agents[2].horse_agent_no == -1 &&
                   w.m5.mission_camera.target_agent_no == -1 &&
                   w.dialog.speaker_agent_no == -1 && w.dialog.pending_mission_agent_no == -1 &&
                   w.player_agent_no == -1 && w.players[1].agent_no == -1 &&
                   w.mission_objects[0].parent_agent_no == -1,
               "remove_agent clears team, AI, offer, horse, camera, dialog, player, and prop refs");
        expect(run_stmt(ctx, 1972U, {0}) && ctx.registers[0] == 0 &&
                   w.teams[0].leader_agent_no == -1 && w.agents[2].horse_agent_no == -1,
               "recycled agent slot 0 inherits no stale references");
    }
    {
        // Mission-object references and 416 keeping scene-placed props.
        VmContext ctx;
        auto& w = ctx.world;
        w.lighting.has_scene = true;
        w.is_multiplayer = true;
        w.misc.num_scene_props = 4;
        w.items.resize(1);
        w.troops.resize(1);
        w.mission_objects.resize(2);  // placed by the scene, not through allocate_mission_object
        w.mission_objects[0].scene_prop_no = 1;
        w.mission_objects[1].scene_prop_no = 2;
        // `multiplayer_clear_scene` (416, `0x140147a80`) deletes agents and prunable objects
        // (`+0x1d5` / `m5.enable_pruning`), resets active players' `agent_no = -1` /
        // `ready_to_spawn = false` (keeping `item_ids`), and leaves unpruned/scene props alone.
        expect(run_stmt(ctx, 1974U, {3, 0}) && ctx.registers[0] == 2, "416: spawn prop 2");
        expect(run_stmt(ctx, 1971U, {0, 0, 5}) && ctx.registers[0] == 3,
               "416: spawn prunable item 3");
        expect(run_stmt(ctx, 1974U, {3, 0}) && ctx.registers[0] == 4, "416: spawn prop 4");
        w.m5.attached_missiles.push_back(wse::game::WseAttachedMissile{2, 0});
        w.m5.attached_missiles.push_back(wse::game::WseAttachedMissile{0, 0});
        w.edit_mode.selected_prop_instances = {2, 0};
        expect(w.remove_mission_object(2) && w.m5.attached_missiles.size() == 1U &&
                   w.m5.attached_missiles[0].mission_object_no == 0 &&
                   w.edit_mode.selected_prop_instances == std::vector<std::int32_t>{0},
               "remove_mission_object drops attached missiles and edit-mode selection");
        expect(run_stmt(ctx, 1972U, {0}), "416: spawn agent");
        w.init_players(2);
        w.connect_player(1, "Tester", 77, 0);
        w.players[1].item_ids[0] = 42;
        w.players[1].m5.ready_to_spawn = true;
        expect(run_stmt(ctx, 416U, {}) && w.agents.empty() &&
                   w.mission_objects.size() == 5U && w.is_valid_mission_object(0) &&
                   w.is_valid_mission_object(1) && !w.is_valid_mission_object(2) &&
                   !w.is_valid_mission_object(3) && w.is_valid_mission_object(4) &&
                   !w.players[1].m5.ready_to_spawn && w.players[1].item_ids[0] == 42,
               "416 removes agents & prunable object 3, resets ready_to_spawn, keeps unpruned props and equipment");
    }
    {
        // step_world_frame consumes queued windows only on the campaign map.
        wse::game::WseWorldState w{};
        const std::array<std::int64_t, 3> t{};
        w.lighting.has_scene = true;
        w.m5.enqueued_windows.emplace_back(wse::game::kQueuedWindowMenuType, -1);
        w.step_world_frame(t, 0.1f);
        expect(w.m5.enqueued_windows.size() == 1U, "mission frames leave the window queue alone");
        w.lighting.has_scene = false;
        w.step_world_frame(t, 0.1f);
        expect(w.m5.enqueued_windows.empty() &&
                   w.m5.active_window == wse::game::kScreenWindowMenu &&
                   w.m6.ui.active_window_id == wse::game::kScreenWindowMenu,
               "campaign frames consume the queued menu window and sync m6.ui.active_window_id");
        // Regression: entering a mission retires the campaign menu window, so the
        // mission-gated WSE opcodes (presentation_activate 4901, 4907, 3044) are not
        // blocked by a stale menu id.
        w.lighting.has_scene = true;
        w.step_world_frame(t, 0.1f);
        expect(w.m6.ui.active_window_id == -1 && w.m5.active_window == -1,
               "mission frames retire a stale campaign menu window");
    }
    {
        // Regression: start_presentation (900) queues its 0x0f window only on the campaign
        // map; in a mission nothing drains the queue.
        VmContext ctx;
        auto& w = ctx.world;
        w.lighting.has_scene = true;
        for (int i = 0; i < 100; ++i) {
            expect(run_stmt(ctx, 900U, {4, 0}), "900 start_presentation in mission");
        }
        expect(w.active_presentation_no == 4 && w.m5.enqueued_windows.empty(),
               "900 in a mission activates the presentation without queueing a window");
        w.lighting.has_scene = false;
        expect(run_stmt(ctx, 900U, {5, 0}) && w.m5.enqueued_windows.size() == 1U &&
                   w.m5.enqueued_windows.back().first ==
                       wse::game::kQueuedWindowPresentationType &&
                   w.m5.enqueued_windows.back().second == 5,
               "900 on the campaign map queues presentation window 0x0f");
    }
    {
        // Regression: scene_prop_set_prune_time (1819) arms pruning like `setPruneTime`
        // (0x140153d88), so 416 removes a spawned prop that was given a prune time.
        VmContext ctx;
        auto& w = ctx.world;
        w.lighting.has_scene = true;
        w.is_multiplayer = true;
        w.misc.num_scene_props = 4;
        expect(run_stmt(ctx, 1974U, {3, 0}) && ctx.registers[0] == 0, "1819: spawn prop 0");
        expect(run_stmt(ctx, 1974U, {3, 0}) && ctx.registers[0] == 1, "1819: spawn prop 1");
        expect(run_stmt(ctx, 1819U, {0, 30}) && w.mission_objects[0].m5.enable_pruning &&
                   w.mission_objects[0].m5.prune_time == 30.0f &&
                   !w.mission_objects[1].m5.enable_pruning,
               "1819 with a positive time enables pruning");
        expect(run_stmt(ctx, 416U, {}) && !w.is_valid_mission_object(0) &&
                   w.is_valid_mission_object(1),
               "416 removes the prop given a prune time and keeps the other");
    }
}

}  // namespace

namespace wse::selftest {

int run_native_m5_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_m5_review_regressions();
    test_agents_m5();
    test_mp_players_m5();
    test_scenes_props_m5();
    test_ui_menus_m5();

    if (g_failures == 0) {
        std::printf("PASS  native_m5_test\n");
        return 0;
    }
    std::printf("FAIL  native_m5_test (%d failures)\n", g_failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_native_m5_suite(nullptr);
}
#endif
