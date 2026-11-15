// Host unit tests for all 204 Milestone 6 WSE opcodes across multiplayer/players,
// agents, scene/mission/terrain, UI/menus/skins, and Lua bridge groups.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include "wse_test_common.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace wse::test;

struct M6Fixture {
    wse::game::WseWorldState world{};
    std::array<std::u16string, 128> str_regs{};
    std::array<std::u16string_view, 16> global_strs{};
    std::array<std::u16string_view, 16> local_strs{};
    std::array<wse::core::WseTransform, 64> pos_regs{};
    std::array<float, 32> fp_regs{};
    std::array<std::int64_t, 128> bank_regs{};
    std::array<std::int64_t, 64> local_vars{};

    M6Fixture() {
        world.has_game = true;
        world.is_multiplayer = true;
        world.is_server = true;
        world.num_skins = 4;
        world.misc.num_menus = 10;
        world.misc.num_actions = 600;
        world.num_particle_systems = 8;

        world.troops.resize(4);
        world.troops[0].name = "Hero";
        world.troops[1].name = "Infantry";

        world.items.resize(6);
        world.items[0].value = 100;
        world.items[0].hit_points = 80;
        world.items[1].properties = 0x07ULL;  // shield
        world.items[1].hit_points = 120;
        world.items[2].properties = 0x02ULL;  // one-handed weapon
        world.items[2].max_ammo = 30;
        world.items[3].properties = 0x01ULL;  // horse
        world.items[3].hit_points = 200;

        world.parties.resize(5);

        world.players.resize(4);
        world.players[0].name = "HostPlayer";
        world.players[0].agent_no = 0;
        world.players[0].privilege = 1;
        world.players[0].unique_id = 1001;
        world.players[1].name = "ClientOne";
        world.players[1].agent_no = 1;
        world.players[1].privilege = 3;
        world.players[1].unique_id = 2002;
        world.players[2].name = "ClientTwo";
        world.players[2].privilege = 4;
        world.players[2].unique_id = 3003;

        world.agents.resize(5);
        for (auto& ag : world.agents) {
            ag.active = true;
            ag.status = 1;
            ag.type = 0;
            ag.is_non_player = true;
            ag.hit_points = 100.0f;
            ag.max_hit_points = 100.0f;
            ag.damage_modifier = 1.0f;
            ag.position = wse::core::WseTransform::identity();
        }
        world.agents[0].player_no = 0;
        world.agents[0].is_non_player = false;
        world.agents[1].player_no = 1;
        world.agents[1].is_non_player = false;
        world.agents[3].type = 1;  // horse agent
        world.agents[4].type = 1;  // secondary horse agent

        world.sites.resize(4);
        world.mission_objects.resize(4);
        for (auto& mo : world.mission_objects) {
            mo.active = true;
            mo.is_item = false;
            mo.scene_prop_no = 1;
            mo.hit_points = 100.0f;
            mo.max_hit_points = 150.0f;
            mo.position = wse::core::WseTransform::identity();
        }

        world.overlays.resize(6);
        world.overlays[0].type = 1;  // button
        world.overlays[1].type = 2;  // slider
        world.overlays[1].val = 77;
        world.overlays[2].type = 4;  // non-checkbox text label
        world.overlays[2].val = 55;
        world.overlays[2].text = u"Hello Overlay";
        world.overlays[3].type = 4;  // checkbox
        world.overlays[3].m5.is_checkbox = true;
        world.overlays[3].val = 1;
        world.overlays[4].type = 8;  // scrollable container
        world.overlays[4].m5.items = {u"Item0", u"Item1"};
        world.overlays[5].type = 9;  // image button

        for (auto& p : pos_regs) {
            p = wse::core::WseTransform::identity();
        }
    }

    WseVmState state(std::int32_t caller = 0) {
        WseVmState st{};
        st.world = &world;
        st.strings.registers = str_regs;
        st.strings.global = global_strs;
        st.strings.local = local_strs;
        st.positions = pos_regs;
        st.fixed_point_multiplier = 1000;
        st.caller = caller;
        return st;
    }
};

void test_player_mp_group() {
    M6Fixture f;
    auto& w = f.world;
    WseEffect eff;

    // 2900 player_set_skin (requires break_warband_compatibility, mp, valid skin)
    w.m6.server.break_warband_compatibility = false;
    expect(run(opcode(WseOpcode::player_set_skin), {1, 2}, {1, 2}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "2900 ignored without break_warband_compatibility");
    w.m6.server.break_warband_compatibility = true;
    expect(run(opcode(WseOpcode::player_set_skin), {1, 2}, {1, 2}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.players[1].profile_gender == 2,
           "2900 sets player skin");
    expect(run(opcode(WseOpcode::player_set_skin), {1, 9}, {1, 9}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "2900 rejects out-of-range skin");

    // 2901 player_stop_controlling_agent
    w.players[1].agent_no = 1;
    w.agents[1].player_no = 1;
    w.agents[1].is_non_player = false;
    expect(run(opcode(WseOpcode::player_stop_controlling_agent), {1}, {1}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.players[1].agent_no == -1 && w.agents[1].is_non_player &&
               w.agents[1].player_no == -1,
           "2901 detaches player from agent");

    // 2902 player_set_banner_id & 2903 player_set_username
    f.str_regs[0] = u"New_User";
    expect(run(opcode(WseOpcode::player_set_banner_id), {1, 14}, {1, 14}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.players[1].banner_no == 14,
           "2902 sets player banner id");
    expect(run(opcode(WseOpcode::player_set_username), {1, 0}, {1, 0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.players[1].name == "New_User",
           "2903 updates player username");

    // 2904 player_temp_ban (privilege <= 2 only, player != 0, minutes >= 1)
    expect(run(opcode(WseOpcode::player_temp_ban), {2, 15}, {2, 15}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "2904 ignored when privilege > 2");
    expect(run(opcode(WseOpcode::player_temp_ban), {1, 15}, {1, 15}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "2904 ignored for privilege 3 client");
    w.players[1].privilege = 2;
    expect(run(opcode(WseOpcode::player_temp_ban), {0, 15}, {0, 15}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "2904 rejects host player 0");
    expect(run(opcode(WseOpcode::player_temp_ban), {1, 15}, {1, 15}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.players[1].status == 0,
           "2904 temp bans eligible player");
    w.players[1].status = 2;  // reconnect player 1 for subsequent player tests

    // 2905 player_get_wse2_version
    w.m6.player(1).wse2_version = 1155;
    expect(run(opcode(WseOpcode::player_get_wse2_version), {0, 1}, {0, 1}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 1155,
           "2905 gets player WSE2 version");

    // 2906 player_get_party_id & 2907 player_set_party_id
    expect(run(opcode(WseOpcode::player_set_party_id), {1, 2}, {1, 2}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::player_get_party_id), {0, 1}, {0, 1}, eff,
                   f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 2 && w.parties[2].player_no == 1,
           "2907/2906 round-trips player party id and syncs party");
    expect(run(opcode(WseOpcode::player_set_party_id), {1, 3}, {1, 3}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.parties[2].player_no == -1 && w.parties[3].player_no == 1,
           "2907 clears previous party link when switching parties");

    // 2908 player_get_anticheat_stat & 2909 player_anticheat_report
    w.m6.anticheat.players[1].float_stats[11] = 2.5f;
    expect(run(opcode(WseOpcode::player_get_anticheat_stat), {0, 1, 11},
               {0, 1, 11}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 2500,
           "2908 scales anticheat stat by fixed_point_multiplier");
    expect(run(opcode(WseOpcode::player_anticheat_report), {1, 5}, {1, 5}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.anticheat.reports.size() == 1,
           "2909 records anticheat report");

    // 3400 multiplayer_send_chat_message_to_player
    w.my_player_no = 1;
    f.str_regs[1] = u"Welcome to WSE2";
    expect(run(opcode(WseOpcode::multiplayer_send_chat_message_to_player),
               {1, 0, 1}, {1, 0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               !w.displayed_messages.empty(),
           "3400 delivers chat message to local player");

    // 3405..3413 net buffer build + 3401/3402 send + loopback read
    expect(run(opcode(WseOpcode::multiplayer_message_init), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3405 inits buffer 0");
    f.str_regs[2] = u"PacketText";
    expect(run(opcode(WseOpcode::multiplayer_message_put_string), {0, 2},
               {0, 2}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3406 puts string");
    expect(run(opcode(WseOpcode::multiplayer_message_put_int), {0, 42, 8},
               {0, 42, 8}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3407 puts 8-bit int");
    f.pos_regs[1].o = {10.0f, 20.0f, 3.0f, 1.0f};
    expect(run(opcode(WseOpcode::multiplayer_message_put_position), {0, 1},
               {0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3408 puts position");
    f.pos_regs[2].o = {7.5f, -4.25f, 1.5f, 1.0f};
    expect(run(opcode(WseOpcode::multiplayer_message_put_coordinate), {0, 2},
               {0, 2}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3409 puts coordinate");

    expect(run(opcode(WseOpcode::multiplayer_send_composite_message_to_player),
               {1, 17, 0}, {1, 17, 0}, eff, f.fp_regs, f.bank_regs,
               f.state()) == WseDispatch::handled_no_store &&
               !w.m6.sent_composite_messages.empty() &&
               w.m6.has_trigger_net_buffer,
           "3401 sends composite message to player and activates loopback");

    expect(run(opcode(WseOpcode::multiplayer_cur_message_get_string), {3},
               {3}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               f.str_regs[3] == u"PacketText",
           "3410 reads string from current net message");
    expect(run(opcode(WseOpcode::multiplayer_cur_message_get_int), {0, 8},
               {0, 8}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 42,
           "3411 reads 8-bit int from current net message");
    expect(run(opcode(WseOpcode::multiplayer_cur_message_get_position), {4},
               {4}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(f.pos_regs[4].o.x, 10.0f, 0.1f) &&
               close(f.pos_regs[4].o.y, 20.0f, 0.1f),
           "3412 reads position from current net message");
    expect(run(opcode(WseOpcode::multiplayer_cur_message_get_coordinate), {5},
               {5}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(f.pos_regs[5].o.x, 7.5f, 0.1f) &&
               close(f.pos_regs[5].o.y, -4.25f, 0.1f),
           "3413 reads coordinate from current net message");

    expect(run(opcode(WseOpcode::multiplayer_message_init), {1}, {1}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::multiplayer_send_composite_message_to_server),
                   {19, 1}, {19, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3402 sends composite message to server");

    // 3403, 3404, 3414, 4225, 4237 multiplayer profiles
    w.mp.profiles.resize(2);
    expect(w.mp.profiles[0].face_keys.from_string(
               "000000001234567800000000abcdef0100000000000000000000000000000000"),
           "profile face keys parsed");
    w.mp.current_profile_no = 0;
    w.m6.profiles[0].name = "Commander";
    w.m6.profiles[0].gender_skin = 2;

    expect(run(opcode(WseOpcode::multiplayer_get_cur_profile), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 0,
           "3403 returns current profile index");
    expect(run(opcode(WseOpcode::multiplayer_get_num_profiles), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 2,
           "3404 returns number of profiles");
    expect(run(opcode(WseOpcode::multiplayer_cur_profile_get_skin), {0}, {0},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 2,
           "3414 returns current profile skin");
    expect(run(opcode(WseOpcode::str_store_multiplayer_profile_name), {4, 0},
               {4, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               f.str_regs[4] == u"Commander",
           "4225 stores profile name");
    expect(run(opcode(WseOpcode::str_store_multiplayer_profile_face_keys),
               {5, 0}, {5, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               !f.str_regs[5].empty(),
           "4237 stores profile face keys");

    // 3415 multiplayer_connect_to_server & 3416 multiplayer_is_campaign
    f.str_regs[6] = u"127.0.0.1:7240";
    f.str_regs[7] = u"pass";
    expect(run(opcode(WseOpcode::multiplayer_connect_to_server), {6, 7, 1},
               {6, 7, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.server.connect_to_server &&
               w.m6.server.stored_connect_address == "127.0.0.1:7240" &&
               w.m6.server.stored_connect_password == "pass",
           "3415 queues connect_to_server");
    w.m6.server.switching_to_campaign = false;
    expect(run(opcode(WseOpcode::multiplayer_is_campaign), {}, {}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::condition_false,
           "3416 false when not mp campaign");
    w.m6.server.switching_to_campaign = true;
    expect(run(opcode(WseOpcode::multiplayer_is_campaign), {}, {}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3416 true when mp campaign");
    w.m6.server.switching_to_campaign = false;

    // 3500..3520, 4227, 4228, 4230 server passwords, rotation, options, anon, IP
    f.str_regs[8] = u"Admin_99";
    f.str_regs[9] = u"Priv_88";
    f.str_regs[10] = u"Rcon_77";
    f.str_regs[11] = u"status";
    f.str_regs[12] = u"bad!pass";
    expect(run(opcode(WseOpcode::server_set_password_admin), {8}, {8}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::str_store_server_password_admin), {13},
                   {13}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               f.str_regs[13] == u"Admin_99",
           "3500/4227 sets and stores admin password");
    expect(run(opcode(WseOpcode::server_set_password_admin), {12}, {12}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::str_store_server_password_admin), {14},
                   {14}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               f.str_regs[14] == u"Admin_99",
           "3500 rejects non-[0-9A-Za-z_] password");
    expect(run(opcode(WseOpcode::server_set_password_private), {9}, {9}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::str_store_server_password_private), {15},
                   {15}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               f.str_regs[15] == u"Priv_88",
           "3501/4228 sets and stores private password");

    expect(run(opcode(WseOpcode::server_map_rotation_get_count), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == -1 &&
               run(opcode(WseOpcode::server_map_rotation_get_index), {0}, {0},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == -1 &&
               run(opcode(WseOpcode::server_map_rotation_set_index), {0}, {0},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::server_map_rotation_get_map), {0, 0},
                   {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == -1 &&
               run(opcode(WseOpcode::server_map_rotation_add_map), {8}, {8},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::server_map_rotation_remove_map), {0}, {0},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3502..3507 dedicated-only map rotation stubs match WSE2");

    expect(run(opcode(WseOpcode::server_set_horse_friendly_fire), {1}, {1}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::server_get_horse_friendly_fire), {0}, {0},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 1,
           "3509/3508 horse friendly fire round-trip");
    expect(run(opcode(WseOpcode::server_set_show_crosshair), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::server_get_show_crosshair), {0}, {0}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 0,
           "3511/3510 show crosshair round-trip");

    // 3512 get_server_option_at_connect
    w.is_server = false;
    w.m6.server.active_server_ip = "10.0.0.5";
    w.m6.server.active_server_port = 7240;
    wse::game::WseM6ServerListEntry srv_entry;
    srv_entry.ip = "10.0.0.5";
    srv_entry.port = 7240;
    srv_entry.module_settings = {11, 22, 77};
    w.m6.server.server_list = {srv_entry};
    expect(run(opcode(WseOpcode::get_server_option_at_connect), {0, 2}, {0, 2},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 77,
           "3512 reads matched server option on connect");
    w.is_server = true;

    expect(run(opcode(WseOpcode::server_set_password_rcon), {10}, {10}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::execute_server_console_command), {11},
                   {11}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3513/3514 dedicated-only RCON/console stubs succeed");

    expect(run(opcode(WseOpcode::add_anonymous_player), {42, 8}, {42, 8}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.server.anonymous_player_names[42] == "Admin_99" &&
               run(opcode(WseOpcode::remove_anonymous_player), {42}, {42}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               !w.m6.server.anonymous_player_names.contains(42) &&
               run(opcode(WseOpcode::add_anonymous_player), {43, 8}, {43, 8},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::clear_anonymous_players), {}, {}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               w.m6.server.anonymous_player_names.empty(),
           "3515..3517 anonymous player management succeeds");

    expect(run(opcode(WseOpcode::server_set_ghost_mode_advanced),
               {1, 1, 0, 1, 1}, {1, 1, 0, 1, 1}, eff, f.fp_regs, f.bank_regs,
               f.state()) == WseDispatch::handled_no_store &&
               w.m6.server.ghost_advanced_enabled &&
               w.m6.server.ghost_disable_free_cam &&
               !w.m6.server.ghost_free_roam &&
               !w.m6.server.ghost_lock_to_team &&
               w.m6.server.ghost_lock_to_agent &&
               w.m6.server.ghost_only_players,
           "3518 sets advanced ghost mode");

    expect(run(opcode(WseOpcode::server_set_anticheat_option), {0, 1}, {0, 1},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::server_get_anticheat_option), {0, 0},
                   {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 1,
           "3519/3520 anticheat option 0..86 round-trip");
    expect(run(opcode(WseOpcode::server_get_anticheat_option), {0, 87}, {0, 87},
               eff, f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "3520 rejects anticheat option >= 87");

    // 4230 str_store_player_ip
    w.m6.player(1).ip_address = "192.168.1.42";
    expect(run(opcode(WseOpcode::str_store_player_ip), {16, 1}, {16, 1}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               f.str_regs[16] == u"192.168.1.42",
           "4230 stores player IP on server");
}

void test_agent_ext_group() {
    M6Fixture f;
    auto& w = f.world;
    w.m5.num_sounds = 32;
    WseEffect eff;

    // 3300 agent_get_item_modifier (horse modifier)
    w.m6.agent(3).horse_item_modifier = 9;
    expect(run(opcode(WseOpcode::agent_get_item_modifier), {0, 3}, {0, 3}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 9,
           "3300 returns horse item modifier");
    expect(run(opcode(WseOpcode::agent_get_item_modifier), {0, 0}, {0, 0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == -1,
           "3300 returns -1 for non-horse agent");

    // 3301 & 3316 agent_{get,set}_item_slot_modifier
    w.agents[0].equipped_items[2] = 2;
    expect(run(opcode(WseOpcode::agent_set_item_slot_modifier), {0, 2, 15},
               {0, 2, 15}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_item_slot_modifier), {0, 0, 2},
                   {0, 0, 2}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 15,
           "3316/3301 item slot modifier round-trip");

    // 3302 agent_get_animation_progress
    w.agents[0].m5.action_channels[1].progress = 0.65f;
    expect(run(opcode(WseOpcode::agent_get_animation_progress), {0, 0, 1},
               {0, 0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 65,
           "3302 returns integer percent animation progress");

    // 3303 agent_get_dna & 3304 agent_get_ground_scene_prop
    w.agents[0].dna = 123456;
    w.agents[0].m5.standing_mission_object_no = 1;
    w.mission_objects[1].is_item = false;
    expect(run(opcode(WseOpcode::agent_get_dna), {0, 0}, {0, 0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 123456 &&
               run(opcode(WseOpcode::agent_get_ground_scene_prop), {0, 0},
                   {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 1,
           "3303/3304 agent DNA & ground scene prop");
    w.mission_objects[1].is_item = true;
    expect(run(opcode(WseOpcode::agent_get_ground_scene_prop), {0, 0}, {0, 0},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == -1,
           "3304 returns -1 when standing on dropped item");

    // 3305, 3306, 3307, 3308, 3311 item slot ammo/hp/wielded/flags
    w.agents[0].equipped_items[1] = 1;  // shield hp=120
    w.agents[0].wielded_items[0] = 2;
    w.agents[0].wielded_items[1] = 1;
    w.m6.agent(0).item_slot_flags[1] = 7;
    expect(run(opcode(WseOpcode::agent_set_item_slot_ammo), {0, 2, -5},
               {0, 2, -5}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.agents[0].item_ammo[2] == 0,
           "3305 clamps negative ammo to 0");
    expect(run(opcode(WseOpcode::agent_set_item_slot_hit_points), {0, 1, 999},
               {0, 1, 999}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_item_slot_hit_points), {0, 0, 1},
                   {0, 0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 120,
           "3307/3306 caps slot HP to item hit_points");
    expect(run(opcode(WseOpcode::agent_get_wielded_item_slot_no), {0, 0, 0},
               {0, 0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 2 &&
               run(opcode(WseOpcode::agent_get_item_slot_flags), {0, 0, 1},
                   {0, 0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 7,
           "3308/3311 wielded slot no & item slot flags");

    // 3309 agent_get_scale & 3310 agent_set_forced_lod
    w.m6.agent(0).scale = 1.25f;
    expect(run(opcode(WseOpcode::agent_get_scale), {0, 0}, {0, 0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 1250 &&
               run(opcode(WseOpcode::agent_set_forced_lod), {0, 3}, {0, 3}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               w.m6.agent(0).forced_lod == 3,
           "3309/3310 scale * fp_mul & forced LOD 0..5");
    expect(run(opcode(WseOpcode::agent_set_forced_lod), {0, 6}, {0, 6}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "3310 rejects LOD >= 6");

    // 3312..3318 AI move target, horse link, simple behavior, accelerate, body meta mesh
    w.m6.agent(0).ai_move_target_position = {11.0f, 22.0f, 3.0f, 1.0f};
    expect(run(opcode(WseOpcode::agent_ai_get_move_target_position), {0, 0},
               {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(f.pos_regs[0].o.x, 11.0f) &&
               close(f.pos_regs[0].o.y, 22.0f),
           "3312 copies AI move target position");

    expect(run(opcode(WseOpcode::agent_set_horse), {0, 3}, {0, 3}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.agents[0].horse_agent_no == 3 && w.agents[3].rider_agent_no == 0,
           "3313 links rider and horse");
    expect(run(opcode(WseOpcode::agent_set_horse), {0, 4}, {0, 4}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.agents[3].rider_agent_no == -1 && w.agents[4].rider_agent_no == 0,
           "3313 detaches previous horse");

    expect(run(opcode(WseOpcode::agent_ai_set_simple_behavior), {0, 4, 2500},
               {0, 4, 2500}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3314 sets AI simple behavior");
    f.pos_regs[1].o = {1.0f, 2.0f, 0.5f, 1.0f};
    expect(run(opcode(WseOpcode::agent_accelerate), {0, 1, 100}, {0, 1, 100},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3315 accelerates agent with ghost timer");
    expect(run(opcode(WseOpcode::agent_body_meta_mesh_set_vertex_keys_time_point),
               {0, 5, 450}, {0, 5, 450}, eff, f.fp_regs, f.bank_regs,
               f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_body_meta_mesh_set_visibility),
                   {0, 5, 0}, {0, 5, 0}, eff, f.fp_regs, f.bank_regs,
                   f.state()) == WseDispatch::handled_no_store,
           "3317/3318 body meta mesh vertex keys & visibility");
    expect(run(opcode(WseOpcode::agent_body_meta_mesh_set_visibility),
               {0, 29, 0}, {0, 29, 0}, eff, f.fp_regs, f.bank_regs,
               f.state()) == WseDispatch::ignored,
           "3318 rejects body part >= 29");

    // 3319..3324 personal animation, default animations, cancel, ranged dmg, stun
    expect(run(opcode(WseOpcode::agent_set_personal_animation), {0, 1, 42},
               {0, 1, 42}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_personal_animation), {0, 0, 1},
                   {0, 0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 42,
           "3319/3320 personal animation round-trip");
    expect(run(opcode(WseOpcode::agent_set_default_animations), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_cancel_current_animation), {0, 1},
                   {0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3321/3322 default animations & cancel animation");
    expect(run(opcode(WseOpcode::agent_get_ranged_damage_modifier), {0, 0},
               {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               run(opcode(WseOpcode::agent_add_stun), {0, 500}, {0, 500}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3323/3324 ranged damage modifier & stun");

    // 3325..3332 deform, footstep sound, horse rotation velocity, vertical speed
    expect(run(opcode(WseOpcode::agent_body_meta_mesh_deform_in_range),
               {0, 2, 10, 30, 500}, {0, 2, 10, 30, 500}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_body_meta_mesh_deform_in_cycle_loop),
                   {0, 2, 5, 25, 400}, {0, 2, 5, 25, 400}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_body_meta_mesh_get_current_deform_progress),
                   {0, 0, 2}, {0, 0, 2}, eff, f.fp_regs, f.bank_regs,
                   f.state()) == WseDispatch::handled &&
               run(opcode(WseOpcode::agent_body_meta_mesh_get_current_deform_frame),
                   {0, 0, 2}, {0, 0, 2}, eff, f.fp_regs, f.bank_regs,
                   f.state()) == WseDispatch::handled,
           "3325..3328 body meta mesh deform controls and getters");
    expect(run(opcode(WseOpcode::agent_set_footstep_sound), {0, 1, 7}, {0, 1, 7},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_horse_rotation_velocity),
                   {0, 3}, {0, 3}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled,
           "3329/3330 footstep sound & horse rotation velocity");
    expect(run(opcode(WseOpcode::agent_set_current_vertical_speed), {0, 350},
               {0, 350}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_current_vertical_speed), {0, 0},
                   {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 350,
           "3332/3331 vertical speed round-trip");

    // 3333..3339, 3342..3348 group pos, ai mesh face group, time speed, kick, prune, toggles, fade, voice, tilt
    expect(run(opcode(WseOpcode::agent_get_position_in_group), {0, 0}, {0, 0},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_current_ai_mesh_face_group),
                   {0, 0}, {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled,
           "3333/3334 group pos & current AI mesh face group");
    expect(run(opcode(WseOpcode::agent_set_time_speed_multiplier), {0, 1500},
               {0, 1500}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_time_speed_multiplier), {0, 0},
                   {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 1500,
           "3335/3336 time speed multiplier round-trip");
    expect(run(opcode(WseOpcode::agent_kick), {0}, {0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_set_dropped_items_prune_time),
                   {0, 20}, {0, 20}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_set_missile_items_prune_time),
                   {0, 15}, {0, 15}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3337/3338/3339 kick & item prune times");
    expect(run(opcode(WseOpcode::agent_set_left_hand_weapon_collision),
               {0, 1}, {0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_ai_set_can_weapon_switch), {0, 1},
                   {0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_ai_set_can_fight), {0, 1}, {0, 1},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_fade_out_advanced), {0, 500},
                   {0, 500}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_fade_in_advanced), {0, 500},
                   {0, 500}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_set_voice_sound), {0, 2, 11},
                   {0, 2, 11}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_set_enable_tilt), {0, 1}, {0, 1},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3342..3348 agent collision/AI/fade/voice/tilt controls");

    // 3349..3360 rider rotation angles, aim, multihit, crush through, trigger disabled, piercing
    expect(run(opcode(WseOpcode::agent_set_rider_rotation_angles), {0, 2, 15000, 1},
               {0, 2, 15000, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_rider_rotation_angles), {0, 0, 2, 0},
                   {0, 0, 2, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value >= 14998 && eff.value <= 15002 &&
               run(opcode(WseOpcode::agent_set_default_rider_rotation_angles),
                   {0}, {0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3349..3351 rider rotation angles round-trip & reset");

    expect(run(opcode(WseOpcode::agent_ai_set_can_aim), {0, 1}, {0, 1}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "3352 sets AI can aim");

    expect(run(opcode(WseOpcode::agent_has_multihit), {0}, {0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::condition_false &&
               run(opcode(WseOpcode::agent_set_can_multihit), {0, 1}, {0, 1},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_has_multihit), {0}, {0}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3353/3354 agent multihit round-trip");

    expect(run(opcode(WseOpcode::agent_set_crush_through), {0, 2}, {0, 2}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_get_crush_through), {0, 0}, {0, 0},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 2 &&
               run(opcode(WseOpcode::agent_set_crush_through), {0, 3}, {0, 3},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::ignored,
           "3355/3356 crush through 0..2 round-trip and range check");

    for (const std::int32_t trig : {-102, -109, -110, -113}) {
        expect(run(opcode(WseOpcode::agent_set_trigger_disabled), {0, trig, 1},
                   {0, trig, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
                   run(opcode(WseOpcode::agent_is_trigger_disabled), {0, trig},
                       {0, trig}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                       WseDispatch::handled_no_store,
               "3357/3358 disables and queries supported agent triggers");
    }
    expect(run(opcode(WseOpcode::agent_set_trigger_disabled), {0, -99, 1},
               {0, -99, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::ignored,
           "3357 rejects unsupported trigger no");

    expect(run(opcode(WseOpcode::agent_has_piercing), {0}, {0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::condition_false &&
               run(opcode(WseOpcode::agent_set_can_piercing), {0, 1}, {0, 1},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::agent_has_piercing), {0}, {0}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3359/3360 agent piercing round-trip");
}

void test_scene_mission_group() {
    M6Fixture f;
    auto& w = f.world;
    WseEffect eff;

    // 3602..3607
    expect(run(opcode(WseOpcode::set_ally_collision_threshold), {20, 80},
               {20, 80}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(w.m6.scene.ally_collision_min, 0.2f) &&
               close(w.m6.scene.ally_collision_max, 0.8f),
           "3602 sets ally collision threshold");
    expect(run(opcode(WseOpcode::set_ally_collision_threshold), {-1, 80},
               {-1, 80}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::ignored,
           "3602 rejects out-of-range percentage");

    expect(run(opcode(WseOpcode::particle_system_remove), {5}, {5}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.scene.removed_particle_systems.size() == 1 &&
               w.m6.scene.removed_particle_systems[0] == 5,
           "3603 records particle system removal");

    w.m6.scene.spectated_agent_no = 2;
    expect(run(opcode(WseOpcode::get_spectated_agent_no), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 2,
           "3604 reads spectated agent no");

    expect(run(opcode(WseOpcode::prop_instance_set_forced_lod), {0, 4}, {0, 4},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.mission_object(0).forced_lod == 4 &&
               run(opcode(WseOpcode::prop_instance_set_forced_lod), {0, 6},
                   {0, 6}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::ignored,
           "3605 prop forced LOD valid and range check");

    expect(run(opcode(WseOpcode::prop_instance_set_variation_id), {0, 17},
               {0, 17}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::prop_instance_set_variation_id_2), {0, 29},
                   {0, 29}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               w.mission_objects[0].variation_id_no == 17 &&
               w.mission_objects[0].variation_id_2_no == 29,
           "3606/3607 sets variation IDs");

    // 3609, 3611..3613, 3624 missiles
    wse::game::WseM6MissileExt missile;
    auto pt0 = wse::core::WseTransform::identity();
    pt0.o = {0.0f, 0.0f, 1.0f, 1.0f};
    auto pt1 = wse::core::WseTransform::identity();
    pt1.o = {0.0f, 10.0f, 2.0f, 1.0f};
    missile.path_points = {pt0, pt1};
    w.m6.missiles[10] = missile;

    expect(run(opcode(WseOpcode::missile_is_valid), {10}, {10}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::missile_is_valid), {99}, {99}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::condition_false,
           "3612 validates missile ID");
    expect(run(opcode(WseOpcode::missile_get_path_point_position), {0, 1, 10},
               {0, 1, 10}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(f.pos_regs[0].o.y, 10.0f) &&
               run(opcode(WseOpcode::missile_get_cur_position), {1, 10},
                   {1, 10}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               close(f.pos_regs[1].o.z, 2.0f),
           "3609/3613 reads missile path point & current position");

    expect(run(opcode(WseOpcode::missile_remove_on_hit), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state(0)) == WseDispatch::ignored &&
               !w.m6.missiles[10].remove_on_hit,
           "3611 ignored outside ti_on_missile_hit (-52)");
    w.m6.trigger_missile_no = 10;
    expect(run(opcode(WseOpcode::missile_remove_on_hit), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state(-52)) == WseDispatch::handled_no_store &&
               w.m6.missiles[10].remove_on_hit,
           "3611 marks active missiles remove_on_hit in -52 trigger");

    expect(run(opcode(WseOpcode::missile_remove), {10}, {10}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::missile_is_valid), {10}, {10}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::condition_false,
           "3624 marks missile removed");

    // 3614..3618 props, camera, particles, lights, sound progress
    expect(run(opcode(WseOpcode::set_prop_collision_threshold), {2, 10, 90},
               {2, 10, 90}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(w.m6.scene.prop_collision_thresholds[2][0], 0.1f) &&
               close(w.m6.scene.prop_collision_thresholds[2][1], 0.9f),
           "3614 stores prop collision threshold");
    w.misc.camera_transform.o = {5.0f, 6.0f, 7.0f, 1.0f};
    expect(run(opcode(WseOpcode::get_camera_position), {2}, {2}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               close(f.pos_regs[2].o.z, 7.0f),
           "3615 copies camera transform");

    w.mission_objects[0].m5.particle_system_nos = {1, 2, 3};
    expect(run(opcode(WseOpcode::prop_instance_remove_particle_system), {0, 2},
               {0, 2}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.mission_objects[0].m5.particle_system_nos.size() == 2 &&
               run(opcode(WseOpcode::prop_instance_remove_particle_system),
                   {0, -1}, {0, -1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               w.mission_objects[0].m5.particle_system_nos.empty(),
           "3616 removes single or all particle systems");

    w.m6.mission_object(0).has_light = true;
    expect(run(opcode(WseOpcode::prop_instance_remove_light), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               !w.m6.mission_object(0).has_light,
           "3617 removes prop light");

    w.mission_objects[0].m5.active_sound_no = 3;
    w.mission_objects[0].m5.sound_playing = true;
    w.m6.mission_object(0).sound_progress = 0.64f;
    expect(run(opcode(WseOpcode::prop_instance_get_sound_progress), {0, 0},
               {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 64,
           "3618 returns sound progress 0..100");
    w.mission_objects[0].m5.active_sound_no = -1;
    w.mission_objects[0].m5.sound_playing = false;
    expect(run(opcode(WseOpcode::prop_instance_get_sound_progress), {0, 0},
               {0, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == -1,
           "3618 returns -1 when not playing");

    // 3620 cast_ray_agents: ray hits nearest capsule; miss returns condition_false
    for (auto& ag : w.agents) {
        ag.status = 0;
    }
    w.agents[1].status = 1;
    w.agents[1].type = 0;
    w.agents[1].position.o = {0.0f, 15.0f, 0.0f, 1.0f};
    w.agents[3].status = 1;
    w.agents[3].type = 1;  // horse nearer
    w.agents[3].position.o = {0.0f, 8.0f, 0.0f, 1.0f};
    f.pos_regs[3] = wse::core::WseTransform::identity();
    f.pos_regs[3].o = {0.0f, 0.0f, 1.0f, 1.0f};
    f.pos_regs[3].f = {0.0f, 1.0f, 0.0f, 0.0f};
    expect(run(opcode(WseOpcode::cast_ray_agents), {0, 4, 3, 20000},
               {0, 4, 3, 20000}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 3 && f.pos_regs[4].o.y > 7.0f &&
               f.pos_regs[4].o.y < 8.0f,
           "3620 hits nearest active agent capsule");
    expect(run(opcode(WseOpcode::cast_ray_agents), {0, 4, 3, 5000},
               {0, 4, 3, 5000}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::condition_false,
           "3620 returns condition_false beyond ray length");

    // 3621, 3623, 3625, 4101
    f.pos_regs[5].o = {2.0f, 3.0f, 0.5f, 1.0f};
    expect(run(opcode(WseOpcode::ai_mesh_face_group_translate), {7, 5}, {7, 5},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(w.m6.scene.ai_mesh_group_translations[7].y, 3.0f),
           "3621 accumulates AI mesh face group translation");
    const float prev_ep_x = w.entry_points[4].position.o.x;
    expect(run(opcode(WseOpcode::shift_entry_point), {4}, {4}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               close(w.entry_points[4].position.o.x, prev_ep_x + 1.0f),
           "3623 shifts entry point");
    expect(run(opcode(WseOpcode::scene_prop_remove), {0}, {0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               !w.mission_objects[0].active &&
               w.m6.mission_object(0).removed,
           "3625 removes active scene prop on server");

    w.ground_level_z = 4.5f;
    expect(run(opcode(WseOpcode::position_align_to_ground), {5, 0, 1},
               {5, 0, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(f.pos_regs[5].o.z, 4.5f) &&
               close(f.pos_regs[5].u.z, 1.0f),
           "4101 aligns position to ground normal and z");

    // 4500..4522 scene setters, terrain generator keys, fog/waves/wind/sky/weather
    f.str_regs[0] = u"outer_desert";
    expect(run(opcode(WseOpcode::scene_set_flags), {1, 0x2a}, {1, 0x2a}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.sites[1].flags == 0x2a &&
               run(opcode(WseOpcode::scene_set_water_level), {1, 3250},
                   {1, 3250}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               close(w.sites[1].water_level, 3.25f) &&
               run(opcode(WseOpcode::scene_set_bounds),
                   {1, -10000, -10000, 10000, 10000},
                   {1, -10000, -10000, 10000, 10000}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               close(w.m6.site(1).min_bound.x, -10.0f) &&
               close(w.m6.site(1).max_bound.y, 10.0f) &&
               run(opcode(WseOpcode::scene_set_outer_terrain), {1, 0}, {1, 0},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               w.m6.site(1).outer_terrain == "outer_desert",
           "4500..4503 scene flags, water level, bounds, outer terrain");

    auto run_pair = [&](WseOpcode op, std::int32_t a, std::int32_t b) {
        expect(run(opcode(op), {a, b}, {a, b}, eff, f.fp_regs, f.bank_regs,
                   f.state()) == WseDispatch::handled_no_store,
               "4504..4517 scene terrain generator mutator succeeds");
    };
    run_pair(WseOpcode::scene_set_terrain_seed, 1, 12);
    run_pair(WseOpcode::scene_set_river_seed, 1, 9);
    run_pair(WseOpcode::scene_set_flora_seed, 1, 7);
    run_pair(WseOpcode::scene_set_deep_water, 1, 1);
    run_pair(WseOpcode::scene_set_place_river, 1, 1);
    run_pair(WseOpcode::scene_set_disable_grass, 1, 1);
    run_pair(WseOpcode::scene_set_valley_size, 1, 50);
    run_pair(WseOpcode::scene_set_hill_height, 1, 40);
    run_pair(WseOpcode::scene_set_ruggedness, 1, 65);
    run_pair(WseOpcode::scene_set_vegetation, 1, 75);
    expect(run(opcode(WseOpcode::scene_set_size), {1, 120, 160}, {1, 120, 160},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store,
           "4514 scene_set_size succeeds");
    run_pair(WseOpcode::scene_set_region_type, 1, 3);
    run_pair(WseOpcode::scene_set_region_detail, 1, 2);
    run_pair(WseOpcode::scene_set_region_variant, 1, 1);

    expect(run(opcode(WseOpcode::scene_set_height_fog), {5000, 2000, 15000},
               {5000, 2000, 15000}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.scene.height_fog_height == 5 &&
               w.m6.scene.height_fog_enabled &&
               close(w.m6.scene.height_fog_strength, 15.0f),
           "4518 sets height fog");
    expect(run(opcode(WseOpcode::scene_set_water_waves), {2500, 50000, 4000},
               {2500, 50000, 4000}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(w.m6.scene.water_wave_amplitude, 2.5f) &&
               close(w.m6.scene.water_wave_scale, 50.0f) &&
               close(w.m6.scene.water_wave_speed, 4.0f),
           "4519 sets water waves");
    expect(run(opcode(WseOpcode::scene_set_flora_wind), {1500, 80000, 12000},
               {1500, 80000, 12000}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(w.m6.scene.flora_wind_amplitude, 1.5f) &&
               close(w.m6.scene.flora_wind_wavelength, 80.0f) &&
               close(w.m6.scene.flora_wind_speed, 12.0f),
           "4520 sets flora wind");
    expect(run(opcode(WseOpcode::scene_rebake_sky_cube), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.scene.sky_cube_rebake_requested,
           "4521 requests sky cube rebake");
    expect(run(opcode(WseOpcode::scene_set_weather_direction), {90}, {90}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(w.m6.scene.weather_direction, 90.0f) &&
               run(opcode(WseOpcode::scene_set_weather_direction), {-1}, {-1},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               close(w.m6.scene.weather_direction, -1000.0f),
           "4522 sets weather direction and negative random sentinel");
}

void test_ui_menu_group() {
    M6Fixture f;
    auto& w = f.world;
    WseEffect eff;

    // 3018 set_forced_lod
    expect(run(opcode(WseOpcode::set_forced_lod), {2}, {2}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.scene.trigger_forced_lod == 2 &&
               run(opcode(WseOpcode::set_forced_lod), {6}, {6}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::ignored,
           "3018 set_forced_lod validates 0..5");

    // 3019, 3033, 3025 URL GET/POST & process
    f.str_regs[0] = u"https://example.com/api";
    f.str_regs[1] = u"WSE2-Agent";
    f.str_regs[2] = u"k=v";
    expect(run(opcode(WseOpcode::send_message_to_url_advanced), {0, 1, 5, 6, 1, 10},
               {0, 1, 5, 6, 1, 10}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::send_post_message_to_url_advanced),
                   {0, 1, 2, 7, 8, 0, 20}, {0, 1, 2, 7, 8, 0, 20}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               w.m6.ui.url_advanced_queue.size() == 2 &&
               !w.m6.ui.url_advanced_queue[0].is_post &&
               w.m6.ui.url_advanced_queue[1].is_post &&
               w.m6.ui.url_advanced_queue[1].timeout_ms == 20000,
           "3019/3033 enqueues URL GET and POST requests");
    expect(run(opcode(WseOpcode::process_advanced_url_messages), {}, {}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.url_advanced_queue[0].processed &&
               w.m6.ui.url_advanced_queue[1].processed,
           "3025 marks queued URL messages processed");

    // 3023, 3024, 3029, 3032, 3039, 3044
    w.m6.ui.order_flag_active = true;
    expect(run(opcode(WseOpcode::order_flag_is_active), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store,
           "3023 true when order flag active");
    w.m6.ui.order_flag_active = false;
    expect(run(opcode(WseOpcode::order_flag_is_active), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::condition_false,
           "3023 false when order flag inactive");

    f.str_regs[3] = u"intro.bik";
    expect(run(opcode(WseOpcode::play_bink_file), {3, 1500}, {3, 1500}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.bink_videos.size() == 1 &&
               w.m6.ui.bink_videos[0].second == 1500,
           "3024 queues bink playback");

    f.str_regs[4] = u"HTTP://example.com";
    f.str_regs[5] = u"file:///etc/passwd";
    expect(run(opcode(WseOpcode::shell_open_url), {4}, {4}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::shell_open_url), {5}, {5}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::ignored &&
               w.m6.ui.shell_opened_urls.size() == 1,
           "3029 enforces scheme allowlist on shell_open_url");

    f.str_regs[6] = u"win.png";
    f.str_regs[7] = u"../escape.png";
    expect(run(opcode(WseOpcode::make_screenshot), {1, 6}, {1, 6}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::make_screenshot), {1, 7}, {1, 7}, eff,
                   f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored &&
               w.m6.ui.screenshots.size() == 1,
           "3032 validates path traversal and format 0..3");

    w.ground_level_z = 2.0f;
    w.m6.ui.cursor_ray_valid = true;
    w.m6.ui.cursor_ray_origin = {3.0f, 4.0f, 10.0f, 1.0f};
    w.m6.ui.cursor_ray_dir = {0.5f, 0.0f, -1.0f, 0.0f};
    expect(run(opcode(WseOpcode::get_mouse_map_coordinates), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(f.pos_regs[0].o.x, 7.0f) &&
               close(f.pos_regs[0].o.z, 2.0f),
           "3039 intersects cursor ray with ground plane");
    w.m6.ui.cursor_ray_dir = {1.0f, 0.0f, 0.0f, 0.0f};
    expect(run(opcode(WseOpcode::get_mouse_map_coordinates), {0}, {0}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "3039 rejects horizontal cursor ray");

    expect(run(opcode(WseOpcode::conversation_screen_auto_update_choice), {},
               {}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.conversation_auto_update_choice,
           "3044 marks conversation auto update");

    // 3100..3102 key bindings and released checks
    w.m6.ui.game_key_mappings[4][1][2] = 0x2A;
    expect(run(opcode(WseOpcode::game_key_get_key), {0, 4, 1, 2}, {0, 4, 1, 2},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 0x2A,
           "3100 reads game_key mapping");

    w.m6.ui.prev_key_down[0x1E] = true;
    w.input.key_down[0x1E] = false;
    w.m6.ui.console_open = true;
    expect(run(opcode(WseOpcode::key_released), {0x1E, 0}, {0x1E, 0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::condition_false &&
               run(opcode(WseOpcode::key_released), {0x1E, 1}, {0x1E, 1}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
           "3101 falling edge + console suppression + bypass");

    w.m6.ui.prev_game_key_down[3] = true;
    w.input.game_key_down[3] = false;
    w.m6.ui.console_open = false;
    expect(run(opcode(WseOpcode::game_key_released), {3}, {3}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store,
           "3102 game_key falling edge");

    // 4229, 4700..4703
    expect(run(opcode(WseOpcode::str_store_overlay_text), {8, 2}, {8, 2}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               f.str_regs[8] == u"Hello Overlay",
           "4229 stores overlay text");

    w.resource_materials["mat_a"] = true;
    w.resource_materials["mat_b"] = true;
    f.str_regs[9] = u"mat a";
    f.str_regs[10] = u"mat b";
    expect(run(opcode(WseOpcode::update_material), {9, 10}, {9, 10}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.updated_materials.size() == 1 &&
               w.m6.ui.updated_materials[0] == "mat_a->mat_b",
           "4700 normalizes spaces and records material update");

    f.str_regs[11] = u"mod_items.txt";
    expect(run(opcode(WseOpcode::reload_item_kinds), {1, 11}, {1, 11}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.reload_item_kinds_requested &&
               w.m6.ui.item_kinds_file == "mod_items.txt" &&
               run(opcode(WseOpcode::reload_troops), {}, {}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.ui.reload_troops_requested &&
               run(opcode(WseOpcode::reload_parties), {}, {}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.ui.reload_parties_requested,
           "4701..4703 sets resource reload flags");

    // 4800..4803 dynamic menus
    f.str_regs[12] = u"Camp Ext";
    f.str_regs[13] = u"mesh_bg";
    f.str_regs[14] = u"Option One";
    expect(run(opcode(WseOpcode::menu_create_new), {0, 12, 13, 0, 3},
               {0, 12, 13, 0, 3}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 10 && w.misc.num_menus == 11,
           "4800 creates dynamic menu and increments num_menus");
    expect(run(opcode(WseOpcode::menu_add_item), {10, 14, 1, 2}, {10, 14, 1, 2},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.menus[10].items.size() == 1,
           "4801 appends menu item");
    expect(run(opcode(WseOpcode::menu_clear_items), {10}, {10}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.ui.menus[10].items.empty(),
           "4802 clears dynamic menu items");
    expect(run(opcode(WseOpcode::menu_clear_generated), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.misc.num_menus == 10 && w.m6.ui.menus.empty(),
           "4803 removes generated menus and restores num_menus");

    // 4900..4907 overlays and presentations
    expect(run(opcode(WseOpcode::overlay_get_val), {0, 1}, {0, 1}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 77,
           "4900 gets slider overlay value");
    expect(run(opcode(WseOpcode::overlay_get_val), {0, 2}, {0, 2}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == -1,
           "4900 returns -1 for non-checkbox text overlay");
    expect(run(opcode(WseOpcode::overlay_get_val), {0, 3}, {0, 3}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 1,
           "4900 returns value for checkbox overlay");

    w.m6.ui.active_presentations = {4};
    expect(run(opcode(WseOpcode::presentation_activate), {4}, {4}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.active_presentation_no == 4 &&
               run(opcode(WseOpcode::presentation_activate), {99}, {99}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::condition_false,
           "4901 activates active presentation");

    expect(run(opcode(WseOpcode::overlay_button_set_type), {0, 0, 1}, {0, 0, 1},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.overlay_ext[0].button_type == 3,
           "4902 sets button type flags");

    expect(run(opcode(WseOpcode::overlay_set_scroll_pos), {4, 2500, 1},
               {4, 2500, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::overlay_get_scroll_pos), {0, 4, 1},
                   {0, 4, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == 2500,
           "4904/4903 overlay scroll pos round-trip");

    expect(run(opcode(WseOpcode::overlay_enable), {0, 0}, {0, 0}, eff,
               f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               !w.m6.ui.overlay_ext[0].enabled,
           "4905 sets overlay enabled flag");
    f.str_regs[15] = u"UpdatedItem";
    expect(run(opcode(WseOpcode::overlay_item_set_text), {4, 1, 15}, {4, 1, 15},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.overlays[4].m5.items[1] == u"UpdatedItem",
           "4906 updates combo/listbox item text");
    expect(run(opcode(WseOpcode::start_conversation_presentation), {7}, {7},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.ui.conversation_presentation_started &&
               w.active_presentation_no == 7,
           "4907 starts conversation presentation");

    // 5200..5202 skins and skeleton body sections
    w.resource_skeleton_models["skel_human"] = true;
    f.str_regs[16] = u"skel_human";
    expect(run(opcode(WseOpcode::skin_set_blood_color),
               {1, static_cast<std::int64_t>(0xffaa0000U)},
               {1, static_cast<std::int32_t>(0xffaa0000U)}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.skins.blood_color(1) == 0xffaa0000U,
           "5200 sets blood color");
    expect(run(opcode(WseOpcode::skeleton_model_set_bone_body_section),
               {16, 12, 2}, {16, 12, 2}, eff, f.fp_regs, f.bank_regs,
               f.state()) == WseDispatch::handled_no_store &&
               w.m6.skins.bone_body_sections["skel_human"][12] == 2 &&
               run(opcode(WseOpcode::skeleton_model_clean_body_sections), {16},
                   {16}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               w.m6.skins.bone_body_sections["skel_human"][12] == 0,
           "5201/5202 sets and cleans skeleton bone body sections");
}

void test_lua_bridge_group() {
    M6Fixture f;
    auto& w = f.world;
    WseEffect eff;

    // 5100..5111 stack operations
    expect(run(opcode(WseOpcode::lua_get_top), {0}, {0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 0,
           "5100 empty Lua stack top is 0");

    f.str_regs[0] = u"alpha";
    f.pos_regs[0].o = {1.5f, 2.5f, 3.5f, 1.0f};
    expect(run(opcode(WseOpcode::lua_push_int), {42}, {42}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_push_str), {0}, {0}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_push_pos), {0}, {0}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_get_top), {0}, {0}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 3,
           "5108..5110/5100 push int/str/pos and get_top == 3");

    expect(run(opcode(WseOpcode::lua_to_int), {0, 1}, {0, 1}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 42 &&
               run(opcode(WseOpcode::lua_to_str), {1, 2}, {1, 2}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               f.str_regs[1] == u"alpha" &&
               run(opcode(WseOpcode::lua_to_pos), {1, -1}, {1, -1}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               close(f.pos_regs[1].o.y, 2.5f),
           "5105..5107 to_int, to_str, and negative-index to_pos");

    expect(run(opcode(WseOpcode::lua_get_type), {0, 2}, {0, 2}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value ==
                   static_cast<std::int64_t>(
                       wse::game::WseLuaValue::Type::string),
           "5111 returns Lua value type");

    expect(run(opcode(WseOpcode::lua_insert), {1}, {1}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_to_pos), {2, 1}, {2, 1}, eff,
                   f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store &&
               close(f.pos_regs[2].o.z, 3.5f),
           "5102 inserts top element at index 1");

    expect(run(opcode(WseOpcode::lua_remove), {1}, {1}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_pop), {1}, {1}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_set_top), {5}, {5}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_get_top), {0}, {0}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 5,
           "5103/5104/5101 remove, pop, set_top resize stack");

    // 5112 lua_call: dot path validation + space-to-underscore + rc < 0
    w.m6.lua.functions["game_sum_two"] = [](wse::game::WseLuaState& lua,
                                            std::int32_t nargs) -> int {
        const auto a = lua.to_integer(-1);
        const auto b = lua.to_integer(-2);
        lua.pop(nargs);
        lua.push_int(a + b);
        return 1;
    };
    w.m6.lua.functions["fail_fn"] = [](wse::game::WseLuaState&,
                                       std::int32_t) -> int { return -1; };
    f.str_regs[2] = u"game sum two";
    f.str_regs[3] = u"bad..path";
    f.str_regs[4] = u"fail_fn";
    expect(run(opcode(WseOpcode::lua_set_top), {0}, {0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_push_int), {10}, {10}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_push_int), {25}, {25}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_call), {2, 2}, {2, 2}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_to_int), {0, -1}, {0, -1}, eff,
                   f.fp_regs, f.bank_regs, f.state()) == WseDispatch::handled &&
               eff.value == 35,
           "5112 normalizes spaces and executes registered Lua function");

    const std::size_t err_before = w.m6.lua.errors.size();
    expect(run(opcode(WseOpcode::lua_call), {3, 0}, {3, 0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.lua.errors.size() > err_before,
           "5112 logs bad dot path");
    expect(run(opcode(WseOpcode::lua_call), {4, 0}, {4, 0}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::condition_false,
           "5112 returns condition_false on negative rc");

    // 5113 lua_triggerCallback & 5114 lua_test
    w.m6.lua.callbacks[7] = [](wse::game::WseLuaState& lua,
                               std::int32_t part) -> int {
        wse::game::WseLuaValue b{};
        b.type = wse::game::WseLuaValue::Type::boolean;
        b.bool_val = (part == 0 && lua.lua_context == 99);
        lua.stack.push_back(b);
        return 1;
    };
    const std::size_t stack_before = w.m6.lua.stack.size();
    expect(run(opcode(WseOpcode::lua_triggerCallback), {7, 0, 99}, {7, 0, 99},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.lua.stack.size() == stack_before,
           "5113 boolean true callback passes and restores stack height");
    expect(run(opcode(WseOpcode::lua_triggerCallback), {7, 0, 12}, {7, 0, 12},
               eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::condition_false &&
               w.m6.lua.stack.size() == stack_before,
           "5113 boolean false callback fails and restores stack height");

    wse::game::WseLuaValue truthy{};
    truthy.type = wse::game::WseLuaValue::Type::boolean;
    truthy.bool_val = true;
    wse::game::WseLuaValue falsy{};
    falsy.type = wse::game::WseLuaValue::Type::boolean;
    falsy.bool_val = false;
    w.m6.lua.stack.push_back(falsy);
    w.m6.lua.stack.push_back(truthy);
    expect(run(opcode(WseOpcode::lua_test), {-1}, {-1}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::lua_test), {-2}, {-2}, eff, f.fp_regs,
                   f.bank_regs, f.state()) == WseDispatch::condition_false,
           "5114 tests Lua boolean truthiness at stack indices");
}

void test_guards_and_boundaries() {
    WseEffect eff;
    WseVmState null_state{};
    expect(run(opcode(WseOpcode::player_set_skin), {}, {}, eff, {}, {},
               null_state) == WseDispatch::operand_count_out_of_range &&
               run(opcode(WseOpcode::agent_get_dna), {0}, {0}, eff, {}, {},
                   null_state) == WseDispatch::operand_count_out_of_range &&
               run(opcode(WseOpcode::scene_set_height_fog), {0, 0}, {0, 0}, eff,
                   {}, {}, null_state) ==
                   WseDispatch::operand_count_out_of_range &&
               run(opcode(WseOpcode::overlay_item_set_text), {0, 0}, {0, 0},
                   eff, {}, {}, null_state) ==
                   WseDispatch::operand_count_out_of_range &&
               run(opcode(WseOpcode::lua_call), {0}, {0}, eff, {}, {},
                   null_state) == WseDispatch::operand_count_out_of_range,
           "M6 handlers reject undersized operand counts");

    const std::int64_t imin = std::numeric_limits<std::int64_t>::min();
    const std::int32_t imin32 = std::numeric_limits<std::int32_t>::min();
    M6Fixture f;
    expect(run(opcode(WseOpcode::player_get_wse2_version), {0, imin},
               {0, imin32}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::ignored &&
               run(opcode(WseOpcode::agent_get_dna), {0, imin}, {0, imin32},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::scene_set_flags), {imin, imin},
                   {imin32, imin32}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::overlay_get_val), {0, imin}, {0, imin32},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::lua_to_int), {0, imin}, {0, imin32}, eff,
                   f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored,
           "INT64_MIN entity and index operands are safely ignored");
}

// Regressions found in the Milestone 6 review.
void test_m6_review_regressions() {
    M6Fixture f;
    auto& w = f.world;
    WseEffect eff;

    // 5101 lua_set_top rejects indices beyond the stack cap instead of
    // allocating a script-controlled amount of memory.
    const std::int32_t int_max = std::numeric_limits<std::int32_t>::max();
    expect(run(opcode(WseOpcode::lua_set_top), {int_max}, {int_max}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored &&
               w.m6.lua.stack.empty(),
           "5101 ignores a set_top beyond kMaxLuaStackSize");
    for (std::size_t i = 0; i < wse::game::WseLuaState::kMaxLuaStackSize + 10U;
         ++i) {
        (void)run(opcode(WseOpcode::lua_push_int), {1}, {1}, eff, f.fp_regs,
                  f.bank_regs, f.state());
    }
    expect(w.m6.lua.stack.size() == wse::game::WseLuaState::kMaxLuaStackSize,
           "5108 stops pushing at kMaxLuaStackSize");
    w.m6.lua.stack.clear();

    // 5112 failed calls do not grow the error log without bound.
    f.str_regs[0] = u"missing_function";
    for (int i = 0; i < 2000; ++i) {
        (void)run(opcode(WseOpcode::lua_call), {0, 0}, {0, 0}, eff, f.fp_regs,
                  f.bank_regs, f.state(), {3});
    }
    expect(w.m6.lua.errors.size() == wse::game::WseLuaState::kMaxLuaErrors,
           "lua error log is capped at kMaxLuaErrors");

    // 3019 timeout seconds -> ms saturates instead of overflowing.
    f.str_regs[1] = u"http://example.invalid/";
    f.str_regs[2] = u"agent";
    expect(run(opcode(WseOpcode::send_message_to_url_advanced),
               {1, 2, -1, -1, 0, 3000000}, {1, 2, -1, -1, 0, 3000000}, eff,
               f.fp_regs, f.bank_regs, f.state(), {3, 3}) ==
               WseDispatch::handled_no_store &&
               !w.m6.ui.url_advanced_queue.empty() &&
               w.m6.ui.url_advanced_queue.back().timeout_ms == int_max,
           "3019 saturates an overflowing timeout");

    // 3611 flags only the missile that fired the trigger.
    w.m6.missiles[1];
    w.m6.missiles[2];
    w.m6.missiles[3];
    w.m6.trigger_missile_no = -1;
    expect(run(opcode(WseOpcode::missile_remove_on_hit), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state(-52)) == WseDispatch::ignored &&
               !w.m6.missiles[1].remove_on_hit,
           "3611 ignored without a trigger missile");
    w.m6.trigger_missile_no = 2;
    expect(run(opcode(WseOpcode::missile_remove_on_hit), {}, {}, eff, f.fp_regs,
               f.bank_regs, f.state(-52)) == WseDispatch::handled_no_store &&
               !w.m6.missiles[1].remove_on_hit &&
               w.m6.missiles[2].remove_on_hit && !w.m6.missiles[3].remove_on_hit,
           "3611 flags only the trigger missile");

    // 3308 returns the equipment slot, not the item kind.
    w.agents[2].equipped_items[3] = 5;
    w.agents[2].wielded_items[0] = 5;
    w.agents[2].wielded_items[1] = -1;
    expect(run(opcode(WseOpcode::agent_get_wielded_item_slot_no), {0, 2, 0},
               {0, 2, 0}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled &&
               eff.value == 3 &&
               run(opcode(WseOpcode::agent_get_wielded_item_slot_no), {0, 2, 1},
                   {0, 2, 1}, eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled &&
               eff.value == -1,
           "3308 reports the slot of the wielded item, -1 for an empty hand");

    // 3332 writes the vertical component and leaves horizontal speed alone.
    w.agents[2].speed = {3.0f, 4.0f, 0.0f, 0.0f};
    expect(run(opcode(WseOpcode::agent_set_current_vertical_speed), {2, 900},
               {2, 900}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               close(w.agents[2].speed.x, 3.0f) &&
               close(w.agents[2].speed.y, 4.0f) &&
               close(w.agents[2].speed.z, 9.0f),
           "3332 sets speed.z without touching horizontal speed");

    // 5200 stores the colour per skin.
    expect(run(opcode(WseOpcode::skin_set_blood_color), {2, 0x112233},
               {2, 0x112233}, eff, f.fp_regs, f.bank_regs, f.state()) ==
               WseDispatch::handled_no_store &&
               w.m6.skins.blood_color(2) == 0x112233U &&
               w.m6.skins.blood_color(1) ==
                   wse::game::WseM6SkinExt::kDefaultBloodColor,
           "5200 changes only the addressed skin");

    // 3313 with an invalid new horse leaves the existing link intact.
    w.agents[0].horse_agent_no = 3;
    w.agents[3].rider_agent_no = 0;
    expect(run(opcode(WseOpcode::agent_set_horse), {0, 99}, {0, 99}, eff,
               f.fp_regs, f.bank_regs, f.state()) == WseDispatch::ignored &&
               w.agents[0].horse_agent_no == 3 && w.agents[3].rider_agent_no == 0,
           "3313 rejects an invalid horse without detaching the old one");

    // 3613 returns an orthonormal basis for a missile flying along +Y.
    {
        auto& m = w.m6.missiles[7];
        wse::core::WseTransform a = wse::core::WseTransform::identity();
        wse::core::WseTransform b = wse::core::WseTransform::identity();
        a.o = {0.0f, 0.0f, 1.0f, 1.0f};
        b.o = {0.0f, 5.0f, 1.0f, 1.0f};
        m.path_points = {a, b};
        expect(run(opcode(WseOpcode::missile_get_cur_position), {3, 7}, {3, 7},
                   eff, f.fp_regs, f.bank_regs, f.state()) ==
                   WseDispatch::handled_no_store,
               "3613 reads missile position");
        const auto& p = f.pos_regs[3];
        expect(close(p.f.y, 1.0f) && close(p.u.z, 1.0f) &&
                   close(std::fabs(p.s.x), 1.0f),
               "3613 basis is orthonormal for a +Y heading");
    }

    // 4518 saturates an out-of-range fog height.
    expect(run(opcode(WseOpcode::scene_set_height_fog),
               {1000000000000000000LL, 100, 100}, {0, 100, 100}, eff, f.fp_regs,
               f.bank_regs, f.state()) == WseDispatch::handled_no_store &&
               w.m6.scene.height_fog_height == int_max,
           "4518 saturates the fog height");
}

}  // namespace

namespace wse::selftest {

int run_wse_m6_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_m6_review_regressions();
    test_player_mp_group();
    test_agent_ext_group();
    test_scene_mission_group();
    test_ui_menu_group();
    test_lua_bridge_group();
    test_guards_and_boundaries();

    if (failures == 0) {
        std::printf("PASS  wse_m6_test\n");
        return 0;
    }
    std::printf("FAIL  wse_m6_test (%d failures)\n", failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_wse_m6_suite(nullptr);
}
#endif
