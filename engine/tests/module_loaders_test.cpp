// Host tests for clean-room v1.1.5.5 Warband/WSE2 module file loaders.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "wse/core/encoding.h"
#include "wse/game/module_asset_loaders.h"
#include "wse/game/module_bundle.h"
#include "wse/game/module_loader_common.h"
#include "wse/game/module_mission_loaders.h"
#include "wse/game/module_world_loaders.h"
#include "wse/io/text_stream.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"
#include "wse/vm/native_ops.h"
#include "wse/vm/operand.h"
#include "wse/vm/wse_opcodes.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

bool expect_ok(const wse::game::ModuleLoadError& err, const char* what) {
    if (!err.ok) {
        ++failures;
        std::printf("FAIL  %s (%s)\n", what, err.format().c_str());
        return false;
    }
    return true;
}

template <class Fn>
auto with_reader(const std::string& text, Fn&& fn) {
    wse::io::TokenReader reader(text);
    return fn(reader);
}

bool close(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

constexpr std::uint64_t tagged(std::uint8_t tag, std::uint64_t payload) noexcept {
    return (static_cast<std::uint64_t>(tag & 0x7FU) << 56) |
           (payload & wse::vm::kPayloadMask);
}

constexpr std::int64_t reg(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(1, static_cast<std::uint64_t>(idx)));
}

constexpr std::int64_t str_bank_global(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(3, static_cast<std::uint64_t>(idx)));
}

constexpr std::int64_t str_bank_quick(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(
        tagged(0x16, static_cast<std::uint64_t>(idx)));
}

std::string build_skills_fixture() {
    std::ostringstream oss;
    oss << "37\n";
    for (int i = 0; i < 37; ++i) {
        if (i == 36) {
            oss << "skl_ironflesh Iron_Flesh 0 7 Increases_max_hit_points\n";
        } else {
            oss << "skl_" << i << " Skill_" << i << " 1 10 Desc_" << i << "\n";
        }
    }
    return oss.str();
}

std::string build_factions_fixture() {
    return "factionsfile version 1\n"
           "2\n"
           "fac_commoners Commoners_Guild 0 16777215 1.0 -0.25 1 Peasants\n"
           "fac_kingdom_1 Kingdom_of_Swadia 4 15618120 -0.25 1.0 2 Vassal King\n";
}

std::string build_troops_fixture(int version) {
    std::ostringstream oss;
    oss << "troopsfile version " << version << "\n2\n";
    for (int t = 0; t < 2; ++t) {
        oss << (t == 0 ? "trp_player Main_Hero Main_Heroes"
                       : "trp_swadian_knight Swadian_Knight Swadian_Knights");
        if (version >= 2) {
            oss << " troop_portrait_" << t;
        }
        oss << "\n";
        // flags (skin 1 on troop 1, hero flag 0x10 on troop 0)
        const std::uint64_t flags = (t == 0) ? 0x10ULL : 0x1ULL;
        const int site_and_entry = (t == 0) ? ((3 << 16) | 12) : ((0 << 16) | 0);
        oss << flags << " " << site_and_entry << " 0 " << t << " "
            << (t == 0 ? 1 : -1) << " -1\n";
        // 64 inventory entries: put item 0 w/ modifier 2 at slot 0 on troop 0
        for (int i = 0; i < 64; ++i) {
            if (t == 0 && i == 0) {
                oss << "0 " << (2U << 24U) << " ";
            } else if (t == 0 && i == 1) {
                oss << "1 " << (99U << 24U) << " ";
            } else {
                oss << "-1 0 ";
            }
        }
        oss << "\n";
        // STR AGI INT CHA, level
        oss << (t == 0 ? "12 10 9 8 5\n" : "15 12 6 7 14\n");
        // 7 proficiencies
        oss << "100 110 120 90 80 70 0\n";
        // 6 uint32 skills: put ironflesh = 9 in skills[4] bits 16..19 on troop 0
        const std::uint32_t sk4 = (t == 0) ? (9U << 16U) : (3U << 16U);
        oss << "0 0 0 0 " << sk4 << " 0\n";
        // 4 + 4 face keys
        oss << "11 22 33 44 55 66 77 88\n";
    }
    return oss.str();
}

std::string build_item_kinds_fixture(int version) {
    std::ostringstream oss;
    oss << "itemsfile version " << version << "\n2\n";
    oss << "itm_war_sword Balanced_War_Sword Balanced_War_Swords "
           "2 war_sword_mesh 0 war_sword_scab 4 "
           "65539 16 250 1 2.5 90 "
           "0 0 0 0 100 99 0 105 0 " << ((2 << 8) | 18) << " " << ((0 << 8) | 30) << "\n";
    if (version > 2) {
        oss << "2 0 1\n";
    }
    if (version > 1) {
        oss << "1\n-50.0 0\n";
    }
    oss << "itm_arrows Bodkin_Arrows Bodkin_Arrows_Pl "
           "1 arrow_mesh 0 "
           "5 0 40 0 3.0 100 "
           "0 0 0 0 0 0 0 0 30 2 0\n";
    if (version > 2) {
        oss << "0\n";
    }
    if (version > 1) {
        oss << "0\n";
    }
    return oss.str();
}

std::string build_party_templates_fixture() {
    return "partytemplatesfile version 1\n"
           "1\n"
           "pt_deserters Deserter_Band 2147483651 0 1 3 "
           "1 4 12 0 "
           "-1 -1 -1 -1 -1\n";
}

std::string build_parties_fixture() {
    // 3 slots (2 active), second active party has a valid stack (troop 1) and
    // an out-of-range stack (troop 99) that should be pruned when world.troops is loaded.
    const std::uint64_t flags0 = (3ULL << 24U) | 5ULL;
    return "partiesfile version 1\n"
           "3 2\n"
           "1 0 10 p_main_party Main_Company " + std::to_string(flags0) + " 1 0 0 1 2 2 -1 -1 "
           "10.0 20.0 11.0 21.0 12.5 22.5 0.5 "
           "1 0 1 0 0 "
           "1.5707963\n"
           "0\n"
           "1 2 12 p_town_1 Praven 15 2 0 1 0 0 0 -1 -1 "
           "1.0 2.0 3.0 4.0 -5.0 6.0 0.0 "
           "2 1 25 3 0 99 5 0 0 "
           "0.0\n";
}

std::string build_sites_fixture() {
    return "scenesfile version 1\n"
           "1\n"
           "scn_praven Praven_Center 0 praven_mesh bo_praven "
           "-100.0 -100.0 100.0 100.0 -1.5 "
           "0x00000001300005000002d0b4000000000000000000000000 "
           "2 1 2 1 18 outer_swadia\n";
}

std::string build_quests_fixture() {
    return "questsfile version 1\n1\n"
           "qst_deliver Deliver_Wine_To_Suno 3 Carry_the_cask_safely.\n";
}

std::string build_info_pages_fixture() {
    return "infopagesfile version 1\n1\n"
           "ip_morale Morale_Guide High_morale_helps_party_speed.\n";
}

std::string build_strings_fixture() {
    return "stringsfile version 1\n2\n"
           "str_hello Welcome_to_Calradia!\n"
           "str_sub Hero_{s2}\n";
}

std::string build_quick_strings_fixture() {
    return "1\n"
           "qstr_fast Quick_Report_Ready\n";
}

std::string build_item_modifiers_fixture() {
    std::ostringstream oss;
    for (int i = 0; i < 43; ++i) {
        if (i == 2) {
            oss << "imod_bent Bent_Item 0.65 1.1\n";
        } else {
            oss << "imod_" << i << " Mod_" << i << " 1.0 1.0\n";
        }
    }
    return oss.str();
}

std::string build_meshes_fixture() {
    return "1\nmesh_banner 0 banner_tex 0.0 0.0 0.0 0.0 0.0 0.0 1.0 1.0 1.0\n";
}

std::string build_actions_fixture() {
    return "1\nact_stand 0 1 1 0.50 anim_idle 0 30 0 0 0.1 0.2 0.3 0.0\n";
}

std::string build_skins_fixture() {
    return "skinsfile version 1\n"
           "2\n"
           "man 0 man_body_m man_calf_l man_hand_l man_head "
           "1 dk_jaw 0 1 -0.5 0.5 Jaw_Width "
           "1 hair_short 1 beard_trim 1 hair_blonde 1 beard_blonde "
           "1 face_man 4294967295 1 2 hair_blonde 4290000000 4285000000 "
           "1 0 snd_man_grunt "
           "skel_human 1.0 0 1 "
           "1 0.5 1 2 0.25 1 0.75 2\n"
           "woman 1 woman_body woman_calf_l woman_hand_l woman_head "
           "0 0 0 0 0 0 0 skel_human 0.95 0 1 0\n";
}

std::string build_sounds_fixture(int version) {
    if (version < 3) {
        return "soundsfile version 2\n"
               "1\nsword_hit.wav 0\n"
               "1\nsnd_sword 1 1 0\n";
    }
    return "soundsfile version 3\n"
           "2\nsword_hit.wav 0 shield.wav 4\n"
           "2\nsnd_sword 1 2 0 100 1 200\nsnd_empty 0 0\n";
}

std::string build_music_fixture() {
    return "2\ntrack_1.ogg 1 0\ntrack_2.ogg 2 4\n";
}

std::string build_particle_systems_fixture() {
    std::ostringstream oss;
    oss << "particlesystemsfile version 1\n1\nps_dust 0 prt_smoke";
    for (int i = 0; i < 35; ++i) {
        oss << " " << (static_cast<float>(i) * 0.5f);
    }
    oss << "\n";
    return oss.str();
}

std::string build_postfx_fixture() {
    std::ostringstream oss;
    oss << "postfxfile version 1\n1\npfx_vivid 1 2";
    for (int i = 0; i < 12; ++i) {
        oss << " " << (0.25f * static_cast<float>(i + 1));
    }
    oss << "\n";
    return oss.str();
}

std::string build_map_icons_fixture() {
    return "mapiconsfile version 1\n1\n"
           "icon_player 1 player_mesh 0.35 0 0.0 0.0 0.1 1 -2.0 1 2133 2 0 1\n";
}

std::string build_scene_props_fixture() {
    return "scenepropsfile version 1\n1\n"
           "spr_gatehouse 8 250 gatehouse_mesh bo_gatehouse 1 -40.0 0\n";
}

std::string build_menus_fixture() {
    return "menusfile version 1\n1\n"
           "mnu_start_game 0 Welcome_Hero! mesh_bg 1 2133 2 1 2 "
           "1 opt_continue 0 Continue_Adventure 0 Door_One\n";
}

std::string build_presentations_fixture() {
    return "presentationsfile version 1\n1\n"
           "prsnt_score 0 -1 1 -60.0 0\n";
}

std::string build_mission_templates_fixture() {
    return "missiontemplatesfile version 1\n1\n"
           "mt_town_visit town_visit_keep_underscore 2 -1 Visit_the_town_square "
           "2 "
           "0 272 0 16 1 2 5 6 "
           "1 16 0 0 2 0 "
           "1 -19.0 0.0 0.0 0 0\n";
}

std::string build_tableau_materials_fixture() {
    return "1\ntab_heraldic 0 heraldic_mat 512 512 0.125 -0.25 1.5 2.75 0\n";
}

std::string build_conversations_fixture(int version) {
    std::ostringstream oss;
    oss << "dialogsfile version " << version << "\n1\n"
        << "dlga_start:end 0 0 0 Hello,_traveler! 1 0";
    if (version > 1) {
        oss << " vo_hello.ogg";
    }
    oss << "\n";
    return oss.str();
}

std::string build_flora_fixture() {
    return "2\n"
           "tree_oak " + std::to_string((1ULL << 22U) | (1ULL << 26U)) + " 1 "
           "oak_mesh bo_oak oak_winter_mesh bo_oak_winter 12.5 0.4\n"
           "bush_small 0 1 bush_mesh 0\n";
}

std::string build_ground_specs_fixture() {
    std::ostringstream oss;
    for (int i = 0; i < 11; ++i) {
        if (i == 0) {
            oss << "gr_turf 4 turf_mat 0.5 turf_multi 0.2 0.6 0.1\n";
        } else {
            oss << "gr_" << i << " 0 mat_" << i << " 1.0 multi_" << i << "\n";
        }
    }
    return oss.str();
}

std::string build_skyboxes_fixture() {
    return "1\n"
           "sky_noon 0 1.0 0.5 0.85 pfx_vivid "
           "1.0 0.9 0.8 0.5 0.6 0.9 0.2 0.2 0.3 150.0 4286611584\n";
}

std::string build_scripts_fixture() {
    // startup_probe reads strings[0], quick_strings[0], troop 0 name, faction 1 name,
    // module_settings["net_config"]["max_players"], game_variables["hero_bonus"],
    // troop 0 xp into reg(1), and item 0 thrust damage type into reg(2).
    std::ostringstream oss;
    oss << "scriptsfile version 1\n1\nstartup_probe -1\n8\n"
        << wse::vm::core_op::kStrStoreString << " 2 0 " << str_bank_global(0) << "\n"
        << wse::vm::core_op::kStrStoreString << " 2 1 " << str_bank_quick(0) << "\n"
        << wse::vm::core_op::kStrStoreTroopName << " 2 2 0\n"
        << wse::vm::core_op::kStrStoreFactionName << " 2 3 1\n"
        << static_cast<std::uint32_t>(wse::vm::WseOpcode::str_store_module_setting)
        << " 3 4 " << str_bank_global(3) << " " << str_bank_global(2) << "\n"
        << static_cast<std::uint32_t>(wse::vm::WseOpcode::str_store_game_variable)
        << " 2 5 " << str_bank_global(4) << "\n"
        << "1515 2 " << reg(1) << " 0\n"
        << "2719 2 " << reg(2) << " 0\n";
    return oss.str();
}

std::string build_full_strings_for_e2e() {
    return "stringsfile version 1\n5\n"
           "str_hello Welcome_to_Calradia!\n"
           "str_sub Unused\n"
           "str_sec net_config\n"
           "str_key max_players\n"
           "str_gvar hero_bonus\n";
}

void write_complete_module(wse::platform::Vfs& vfs, std::string_view dir) {
    const auto put = [&](std::string_view rel, std::string_view content) {
        const std::string p = wse::game::vfs_join_path(dir, rel);
        expect(vfs.write_text(p, content), p.c_str());
    };
    put("module.ini",
        "module_name = Native_Clean\n"
        "module_version = 1155 # dev build\n"
        "load_resource = core_textures\n"
        "load_mod_resource = custom_armor\n");
    put("module_config.ini",
        "\xEF\xBB\xBF# user overrides\n"
        "[ net_config ]\n"
        "max_players = 64 // capped\n");
    put("game_variables.txt",
        "hero_bonus = 250 ; overridden from Native\n");
    put("skills.txt", build_skills_fixture());
    put("factions.txt", build_factions_fixture());
    put("troops.txt", build_troops_fixture(2));
    put("item_kinds1.txt", build_item_kinds_fixture(3));
    put("party_templates.txt", build_party_templates_fixture());
    put("parties.txt", build_parties_fixture());
    put("scenes.txt", build_sites_fixture());
    put("quests.txt", build_quests_fixture());
    put("info_pages.txt", build_info_pages_fixture());
    put("strings.txt", build_full_strings_for_e2e());
    put("quick_strings.txt", build_quick_strings_fixture());
    put("scripts.txt", build_scripts_fixture());
    put("triggers.txt", "triggersfile version 1\n1\n-52.0 0.0 0.0 0 0\n");
    put("simple_triggers.txt", "simple_triggersfile version 1\n1\n24.0 0\n");
    put("meshes.txt", build_meshes_fixture());
    put("actions.txt", build_actions_fixture());
    put("skins.txt", build_skins_fixture());
    put("sounds.txt", build_sounds_fixture(3));
    put("music.txt", build_music_fixture());
    put("particle_systems.txt", build_particle_systems_fixture());
    put("postfx.txt", build_postfx_fixture());
    put("map_icons.txt", build_map_icons_fixture());
    put("scene_props.txt", build_scene_props_fixture());
    put("menus.txt", build_menus_fixture());
    put("presentations.txt", build_presentations_fixture());
    put("mission_templates.txt", build_mission_templates_fixture());
    put("tableau_materials.txt", build_tableau_materials_fixture());
    put("conversation.txt", build_conversations_fixture(2));
    put("Data/item_modifiers.txt", build_item_modifiers_fixture());
    put("Data/flora_kinds.txt", build_flora_fixture());
    put("Data/ground_specs.txt", build_ground_specs_fixture());
    put("Data/skyboxes.txt", build_skyboxes_fixture());
}

void test_ini_and_token_helpers() {
    using namespace wse::game;

    std::unordered_map<std::string, std::string> vars;
    parse_sectionless_ini(
        "\xEF\xBB\xBF  alpha = 10 // trailing\r\n"
        "beta = hello ; comment\n"
        "gamma = 99 # hash\n"
        "# full line comment\n"
        "  delta  =  value with spaces  \n",
        vars);
    expect(vars["alpha"] == "10" && vars["beta"] == "hello" &&
               vars["gamma"] == "99" && vars["delta"] == "value with spaces",
           "sectionless INI strips BOM, comments, and whitespace");

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> ini;
    parse_ini_sections(
        "\xEF\xBB\xBForphan = yes\n"
        "[ Audio ]\n"
        "volume = 0.75 ; mid\n"
        "[Net]\n"
        "port = 7240 // default\n",
        ini);
    expect(ini[""]["orphan"] == "yes" && ini["Audio"]["volume"] == "0.75" &&
               ini["Net"]["port"] == "7240",
           "sectioned INI parses default and named sections");

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> ini2;
    parse_ini_sections(
        "[a]\n"
        "x = 1\n"
        "[b] ; trailing comment\n"
        "y = 2\n"
        "[ c ]   // another\n"
        "z = 3\n"
        "; w = [d]\n",
        ini2);
    expect(ini2.size() == 3U && ini2["a"].size() == 1U && ini2["a"]["x"] == "1" &&
               ini2["b"]["y"] == "2" && ini2["c"]["z"] == "3",
           "section headers with trailing comments still open their section");

    ModuleIniData mod_ini;
    parse_module_ini(
        "module_name = MyMod # comment\n"
        "load_resource = common_res\n"
        "load_mod_resource = my_armor\n"
        "load_module_resource = my_weapons\n",
        mod_ini);
    expect(mod_ini.settings["module_name"] == "MyMod" &&
               mod_ini.load_resources.size() == 1U &&
               mod_ini.load_resources[0] == "common_res" &&
               mod_ini.load_mod_resources.size() == 2U,
           "module.ini parses settings and ordered resource directives");

    wse::io::TokenReader tok("-1 2147483648 4294967295 12x 99999999999999 68451502092");
    std::int32_t s = 0;
    std::uint32_t u = 0;
    expect(read_i32_token(tok, s) && s == -1, "signed negative token parses");
    expect(read_i32_token(tok, s) && static_cast<std::uint32_t>(s) == 2147483648U,
           "unsigned 32-bit value fits retail readInt32");
    expect(read_u32_token(tok, u) && u == 4294967295U, "max u32 parses");
    expect(!read_i32_token(tok, s), "trailing garbage rejected");
    expect(!read_i32_token(tok, s), "huge out-of-range integer rejected");
    expect(read_wrapping_u32_token(tok, u) &&
               u == static_cast<std::uint32_t>(68451502092ULL),
           "wrapping u32 mirrors 0x1402afce0 for >32-bit retail colors");

    wse::io::TokenReader neg_tok("-2147483648 -1 -0 - 4294967296 1x");
    expect(read_wrapping_u32_token(neg_tok, u) && u == 0x80000000U,
           "wrapping u32 negates INT32_MIN magnitude without signed overflow");
    expect(read_wrapping_u32_token(neg_tok, u) && u == 0xffffffffU,
           "wrapping u32 maps -1 to 0xffffffff");
    expect(read_wrapping_u32_token(neg_tok, u) && u == 0U,
           "wrapping u32 maps -0 to 0");
    expect(!read_wrapping_u32_token(neg_tok, u), "wrapping u32 rejects bare '-'");
    expect(read_wrapping_u32_token(neg_tok, u) && u == 0U,
           "wrapping u32 wraps 2^32 to 0");
    expect(!read_wrapping_u32_token(neg_tok, u),
           "wrapping u32 rejects trailing garbage");

    // readUInt64 (0x1402afbe0) -> toUInt64Stream (0x14030f7b0) wraps modulo 2^64; the
    // 43-digit token is `itm_drow_c_dagger`'s properties in Phantasy Calradia.
    wse::io::TokenReader u64_tok(
        "18446744073709551615 18446744073709551616 4355614296588012332331194975126633110896642 "
        "12x -1 007");
    std::uint64_t u64 = 0;
    expect(u64_tok.read_u64(u64) && u64 == 0xffffffffffffffffULL, "max u64 parses");
    expect(u64_tok.read_u64(u64) && u64 == 0U, "u64 wraps 2^64 to 0 like toUInt64Stream");
    expect(u64_tok.read_u64(u64) && u64 == 0x410002ULL,
           "43-digit Module System flags wrap modulo 2^64");
    expect(!u64_tok.read_u64(u64), "u64 rejects trailing garbage");
    expect(!u64_tok.read_u64(u64), "u64 rejects a sign");
    expect(u64_tok.read_u64(u64) && u64 == 7U, "u64 accepts leading zeros");

    expect(initial_troop_xp(5, nullptr) == kCampaignLevelXpTable[4],
           "deterministic XP equals g_levelTable[level]");
    std::uint32_t seed = 12345U;
    const std::int32_t seeded_xp = initial_troop_xp(5, &seed);
    expect(seeded_xp >= kCampaignLevelXpTable[4] &&
               seeded_xp <= kCampaignLevelXpTable[5],
           "PRNG-seeded troop XP falls inside [level, level+1] band");
}

void test_individual_world_loaders() {
    using namespace wse::game;
    WseWorldState world{};

    // Skills
    {
        if (expect_ok(with_reader(build_skills_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_skills(r, world);
                                  }),
                      "skills.txt loads")) {
            expect(world.skills[36].id == "skl_ironflesh" &&
                       world.skills[36].name == "Iron Flesh" &&
                       world.skills[36].max_level == 7 &&
                       world.skills[41].flags == 0x100U &&
                       world.skills[41].max_level == 0,
                   "skills populate underscores->spaces and zero remaining < 42");
        }
    }

    // Factions
    {
        std::vector<LoadedFactionMeta> meta;
        if (expect_ok(with_reader(build_factions_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_factions(r, world, &meta);
                                  }),
                      "factions.txt loads")) {
            expect(world.factions.size() == 2U &&
                       world.factions[1].name == "Kingdom of Swadia" &&
                       world.factions[1].color == (15618120U | 0xff000000U) &&
                       close(world.factions[1].relations[0], -0.25f) &&
                       meta[1].ranks.size() == 2U && meta[1].ranks[1] == "King",
                   "factions decode alpha channel, relations matrix, and ranks");
        }
    }

    // Troops v1 and v2
    {
        std::vector<LoadedTroopMeta> meta1;
        if (expect_ok(with_reader(build_troops_fixture(1),
                                  [&](wse::io::TokenReader& r) {
                                      return load_troops(r, world, nullptr, &meta1);
                                  }),
                      "troops v1 loads")) {
            expect(meta1.size() == 2U && meta1[0].image_mesh_name == "0",
                   "troops v1 defaults image_mesh_name to '0'");
        }

        std::vector<LoadedTroopMeta> meta2;
        if (expect_ok(with_reader(build_troops_fixture(2),
                                  [&](wse::io::TokenReader& r) {
                                      return load_troops(r, world, nullptr, &meta2);
                                  }),
                      "troops v2 loads") &&
            world.troops.size() == 2U && meta2.size() == 2U) {
            const auto& tr0 = world.troops[0];
            // Ironflesh raw 9 clamped to skills[36].max_level (7) -> hp = 12 + 35 + 14 = 61
            expect(meta2[0].id == "trp_player" && tr0.name == "Main Hero" &&
                       tr0.plural_name == "Main Heroes" &&
                       meta2[0].image_mesh_name == "troop_portrait_0" &&
                       tr0.home_site_no == 12 && tr0.home_entry_no == 3 &&
                       tr0.inventory[0].is_empty() && tr0.inventory[9].is_empty() &&
                       tr0.inventory[10].item_no == 0 &&
                       tr0.inventory[10].modifier == 2 &&
                       tr0.inventory[11].item_no == 1 &&
                       tr0.inventory[11].modifier == 0 &&
                       close(tr0.hit_points, 61.0f) &&
                       tr0.face_keys[1][3] == 88ULL && world.troops[1].skin_no == 1,
                   "troops populate inventory[10..], clamped ironflesh HP, home site, skin");
        }
    }

    // Item kinds v1, v2, v3
    {
        if (expect_ok(with_reader(build_item_kinds_fixture(1),
                                  [&](wse::io::TokenReader& r) {
                                      return load_item_kinds(r, world);
                                  }),
                      "item_kinds v1 loads")) {
            expect(world.items.size() == 2U && world.items[0].factions.empty(),
                   "item_kinds v1 loads cleanly without factions/triggers");
        }

        std::vector<LoadedItemTriggers> it_trigs;
        if (expect_ok(with_reader(build_item_kinds_fixture(3),
                                  [&](wse::io::TokenReader& r) {
                                      return load_item_kinds(r, world, &it_trigs);
                                  }),
                      "item_kinds v3 loads")) {
            expect(world.items.size() == 2U &&
                       world.items[0].name == "Balanced War Sword" &&
                       world.items[0].variations.size() == 2U &&
                       world.items[0].variations[1].mesh_name == "war_sword_scab" &&
                       world.items[0].factions.size() == 2U &&
                       world.items[0].get_thrust_damage_type() == 2 &&
                       it_trigs.size() == 1U &&
                       it_trigs[0].triggers.triggers.size() == 1U,
                   "item_kinds v3 parses variations, factions, damage, and inline triggers");
        }
    }

    // Party templates & parties
    {
        if (expect_ok(with_reader(build_party_templates_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_party_templates(r, world);
                                  }),
                      "party_templates loads")) {
            expect(world.party_templates.size() == 1U &&
                       world.party_templates[0].icon_no == 3 &&
                       world.party_templates[0].stacks[0].troop_no == 1 &&
                       world.party_templates[0].stacks[1].troop_no == -1,
                   "party_templates loads sparse 6-slot stacks and icon_no");
        }

        std::vector<LoadedPartyMeta> pmeta;
        if (expect_ok(with_reader(build_parties_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_parties(r, world, &pmeta);
                                  }),
                      "parties.txt loads")) {
            expect(world.parties.size() == 3U && world.parties[0].active &&
                       !world.parties[1].active && world.parties[2].active &&
                       world.parties[0].name == "Main Company" &&
                       world.parties[0].icon_no == 5 &&
                       world.parties[0].bandit_attraction == 10 &&
                       close(world.parties[0].position.o.x, 12.5f) &&
                       close(world.parties[0].position.s.y, 1.0f, 1e-3f) &&
                       world.parties[2].stacks.size() == 1U &&
                       world.parties[2].stacks[0].troop_no == 1,
                   "parties handles inactive hash slots, Z-rotation bearing, and prunes invalid troop stacks");
        }
    }

    // Sites, quests, info_pages, item_modifiers
    {
        std::vector<LoadedSiteMeta> smeta;
        if (expect_ok(with_reader(build_sites_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_sites(r, world, &smeta);
                                  }),
                      "scenes.txt loads")) {
            expect(world.sites.size() == 1U &&
                       world.m6.site(0).outer_terrain == "outer_swadia" &&
                       close(world.sites[0].water_level, -1.5f) &&
                       smeta[0].passages.size() == 2U && smeta[0].chests[0] == 18,
                   "scenes.txt loads site bounds, water level, passages, and m6 outer_terrain");
        }

        if (expect_ok(with_reader(build_quests_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_quests(r, world);
                                  }),
                      "quests.txt loads")) {
            expect(world.quests.size() == 1U &&
                       world.quests[0].title == "Deliver Wine To Suno" &&
                       world.quests[0].template_flags == 3U &&
                       world.quests[0].progression_flags == 0U,
                   "quests.txt populates template_flags separately from progression_flags");
        }

        if (expect_ok(with_reader(build_info_pages_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_info_pages(r, world);
                                  }),
                      "info_pages.txt loads")) {
            expect(world.info_pages.size() == 1U &&
                       world.info_pages[0].title == "Morale Guide",
                   "info_pages.txt loads title/text with spaces");
        }

        std::vector<LoadedItemModifier> mods;
        if (expect_ok(with_reader(build_item_modifiers_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_item_modifiers(r, mods);
                                  }),
                      "item_modifiers.txt loads")) {
            expect(mods.size() == 43U &&
                       mods[2].id == "imod_bent" && mods[2].name == "Bent Item" &&
                       close(mods[2].price_multiplier, 0.65f),
                   "item_modifiers.txt reads 43 fixed entries");
        }

        std::vector<LoadedStringEntry> str_entries;
        if (expect_ok(with_reader(build_strings_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_strings(r, str_entries);
                                  }),
                      "strings.txt unit loads")) {
            expect(str_entries.size() == 2U &&
                       str_entries[0].id == "str_hello" &&
                       str_entries[1].text == "Hero {s2}",
                   "strings.txt unit fixture decodes underscores");
        }

        std::vector<LoadedStringEntry> qstr_entries;
        if (expect_ok(with_reader(build_quick_strings_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_quick_strings(r, qstr_entries);
                                  }),
                      "quick_strings.txt unit loads")) {
            expect(qstr_entries.size() == 1U &&
                       qstr_entries[0].id == "qstr_fast" &&
                       qstr_entries[0].text == "Quick Report Ready",
                   "quick_strings.txt unit fixture decodes underscores");
        }
    }

    // Game variables Native + mod fallback
    {
        wse::platform::MemoryVfs vfs;
        expect(vfs.write_text("Modules/Native/game_variables.txt",
                              "base_only = 7\nhero_bonus = 50\n"),
               "write Native game_variables");
        expect(vfs.write_text("Modules/ModX/game_variables.txt",
                              "hero_bonus = 200\n"),
               "write ModX game_variables");
        load_game_variables(vfs, "Modules/ModX", "Modules/Native", world);
        expect(world.game_variables["base_only"] == "7" &&
                   world.game_variables["hero_bonus"] == "200",
               "game_variables layers Native defaults beneath module overrides");
    }
}

void test_asset_mission_and_data_loaders() {
    using namespace wse::game;
    WseWorldState world{};

    {
        std::vector<LoadedMesh> out;
        if (expect_ok(with_reader(build_meshes_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_meshes(r, world, &out);
                                  }),
                      "meshes.txt loads")) {
            expect(world.mesh_names.size() == 1U &&
                       world.mesh_names[0] == "mesh_banner" &&
                       out.size() == 1U && close(out[0].transform[8], 1.0f),
                   "meshes.txt fields");
        }
    }
    {
        std::vector<LoadedAction> out;
        if (expect_ok(with_reader(build_actions_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_actions(r, world, &out);
                                  }),
                      "actions.txt loads")) {
            expect(world.misc.num_actions == 1 && out.size() == 1U &&
                       out[0].anims.size() == 1U &&
                       out[0].anims[0].anim_name == "anim_idle",
                   "actions.txt fields");
        }
    }
    {
        std::vector<LoadedSkin> out;
        if (expect_ok(with_reader(build_skins_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_skins(r, world, &out);
                                  }),
                      "skins.txt loads")) {
            expect(world.num_skins == 2 && out.size() == 2U &&
                       out[0].deform_keys.size() == 1U &&
                       out[0].deform_keys[0].name == "Jaw Width" &&
                       out[0].face_textures.size() == 1U &&
                       out[0].face_textures[0].gradient_colors.size() == 2U &&
                       out[0].constraints.size() == 1U &&
                       out[0].constraints[0].pairs.size() == 2U &&
                       out[0].constraints[0].pairs[1].key_index == 2,
                   "skins.txt loads deform keys, face gradients, voices, and constraints");
        }
    }
    {
        LoadedSoundBank b2;
        if (expect_ok(with_reader(build_sounds_fixture(2),
                                  [&](wse::io::TokenReader& r) {
                                      return load_sounds(r, world, &b2);
                                  }),
                      "sounds.txt v2 loads")) {
            expect(world.m5.num_sounds == 1 && b2.sounds.size() == 1U &&
                       b2.sounds[0].refs.size() == 1U &&
                       b2.sounds[0].refs[0].flags == 0U,
                   "sounds.txt v2 reads refs without per-ref flags");
        }

        LoadedSoundBank b3;
        if (expect_ok(with_reader(build_sounds_fixture(3),
                                  [&](wse::io::TokenReader& r) {
                                      return load_sounds(r, world, &b3);
                                  }),
                      "sounds.txt v3 loads")) {
            expect(world.m5.num_sounds == 2 && b3.sounds.size() == 2U &&
                       b3.sounds[0].refs.size() == 2U &&
                       b3.sounds[0].refs[1].flags == 200U,
                   "sounds.txt v3 reads per-ref flags");
        }
    }
    {
        std::vector<LoadedMusicTrack> out;
        if (expect_ok(with_reader(build_music_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_music(r, world, &out);
                                  }),
                      "music.txt loads")) {
            expect(world.misc.num_music_tracks == 2 && out.size() == 2U &&
                       out[1].continue_flags == 4U,
                   "music.txt fields");
        }
    }
    {
        std::vector<LoadedParticleSystem> out;
        if (expect_ok(with_reader(build_particle_systems_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_particle_systems(r, world, &out);
                                  }),
                      "particle_systems.txt loads")) {
            expect(world.num_particle_systems == 1 && out.size() == 1U &&
                       close(out[0].params[34], 17.0f),
                   "particle_systems.txt reads 35 floats");
        }
    }
    {
        std::vector<LoadedPostEffect> out;
        if (expect_ok(with_reader(build_postfx_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_post_effects(r, world, &out);
                                  }),
                      "postfx.txt loads")) {
            expect(world.misc.num_post_effects == 1 && out.size() == 1U &&
                       out[0].tonemap_op == 2 && close(out[0].params[11], 3.0f),
                   "postfx.txt reads 12 floats");
        }
    }
    {
        std::vector<LoadedMapIcon> out;
        if (expect_ok(with_reader(build_map_icons_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_map_icons(r, world, &out);
                                  }),
                      "map_icons.txt loads")) {
            expect(world.misc.num_map_icons == 1 && out.size() == 1U &&
                       out[0].triggers.triggers.size() == 1U,
                   "map_icons.txt fields");
        }
    }
    {
        std::vector<LoadedSceneProp> out;
        if (expect_ok(with_reader(build_scene_props_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_scene_props(r, world, &out);
                                  }),
                      "scene_props.txt loads")) {
            expect(world.misc.num_scene_props == 1 && out.size() == 1U &&
                       out[0].triggers.defaultTimerNo == 2 &&
                       out[0].triggers.triggers.size() == 1U,
                   "scene_props.txt loads with timer 2");
        }
    }
    {
        std::vector<LoadedMenu> out;
        if (expect_ok(with_reader(build_menus_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_menus(r, world, &out);
                                  }),
                      "menus.txt loads")) {
            expect(world.misc.num_menus == 1 && out.size() == 1U &&
                       out[0].text == "Welcome Hero!" &&
                       out[0].options.size() == 1U &&
                       out[0].options[0].text == "Continue Adventure" &&
                       out[0].options[0].door_name == "Door One",
                   "menus.txt loads options, blocks, and door names");
        }
    }
    {
        std::vector<LoadedPresentation> out;
        if (expect_ok(with_reader(build_presentations_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_presentations(r, out);
                                  }),
                      "presentations.txt loads")) {
            expect(out.size() == 1U && out[0].triggers.triggers.size() == 1U,
                   "presentations.txt fields");
        }
    }
    {
        std::vector<LoadedMissionTemplateRecord> out;
        if (expect_ok(with_reader(build_mission_templates_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_mission_templates(r, world, {},
                                                                    &out);
                                  }),
                      "mission_templates.txt loads")) {
            expect(world.mission_templates.size() == 1U &&
                       world.mission_templates[0].name ==
                           "town_visit_keep_underscore" &&
                       out.size() == 1U &&
                       out[0].description == "Visit the town square" &&
                       !world.mission_templates[0].entry_overrides.empty() &&
                       world.mission_templates[0].entry_overrides[0].override_items ==
                           std::vector<std::int32_t>{5, 6} &&
                       out[0].triggers.triggers.size() == 1U,
                   "mission_templates preserved name underscores and decoded desc + overrides");
        }
    }
    {
        std::vector<LoadedTableauMaterial> out;
        if (expect_ok(with_reader(build_tableau_materials_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_tableau_materials(r, world, &out);
                                  }),
                      "tableau_materials.txt loads")) {
            expect(world.misc.num_tableau_materials == 1 && out.size() == 1U &&
                       close(out[0].min_x, 0.125f) &&
                       close(out[0].max_y, 2.75f),
                   "tableau_materials.txt reads float bounds");
        }
    }
    {
        std::vector<LoadedConversationSentence> c1;
        if (expect_ok(with_reader(build_conversations_fixture(1),
                                  [&](wse::io::TokenReader& r) {
                                      return load_conversations(r, c1);
                                  }),
                      "conversation.txt v1 loads")) {
            expect(c1.size() == 1U && c1[0].voiceover.empty(),
                   "conversation.txt v1 omits voiceover");
        }

        std::vector<LoadedConversationSentence> c2;
        if (expect_ok(with_reader(build_conversations_fixture(2),
                                  [&](wse::io::TokenReader& r) {
                                      return load_conversations(r, c2);
                                  }),
                      "conversation.txt v2 loads")) {
            expect(c2.size() == 1U && c2[0].text == "Hello, traveler!" &&
                       c2[0].voiceover == "vo_hello.ogg",
                   "conversation.txt v2 reads voiceover");
        }
    }
    {
        std::vector<LoadedFloraKind> flora;
        if (expect_ok(with_reader(build_flora_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_flora_kinds(r, flora);
                                  }),
                      "flora_kinds.txt loads")) {
            expect(flora.size() == 2U && !flora[0].instances.empty() &&
                       flora[0].instances[0].winter_mesh_name == "oak_winter_mesh" &&
                       close(flora[0].colony_radius, 12.5f),
                   "flora_kinds.txt decodes winter + colony flag bits");
        }

        std::vector<LoadedGroundSpec> specs;
        if (expect_ok(with_reader(build_ground_specs_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_ground_specs(r, specs);
                                  }),
                      "ground_specs.txt loads")) {
            expect(specs.size() == 11U && close(specs[0].color[1], 0.6f),
                   "ground_specs.txt loads 11 entries + optional RGB");
        }

        std::vector<LoadedSkybox> skies;
        if (expect_ok(with_reader(build_skyboxes_fixture(),
                                  [&](wse::io::TokenReader& r) {
                                      return load_skyboxes(r, world, &skies);
                                  }),
                      "skyboxes.txt loads")) {
            expect(skies.size() == 1U && close(skies[0].flare_strength, 0.85f) &&
                       world.m5.skybox_fog_colors[0] == 4286611584U,
                   "skyboxes.txt loads and seeds skybox_fog_colors");
        }
    }
}

void test_error_diagnostics() {
    using namespace wse::game;

    wse::platform::MemoryVfs vfs;
    wse::vm::VmContext ctx{};
    ModuleBundle bundle{};
    expect(!load_module(vfs, "Modules/Empty", ctx, bundle).ok,
           "strict load_module fails when core files are absent");

    WseWorldState world{};
    {
        wse::io::TokenReader huge("1000000 bad");
        const auto err = load_factions(huge, world);
        expect(!err.ok && !err.format().empty(),
               "corrupt large count fails safely without bad_alloc");
    }
    {
        wse::io::TokenReader neg("-3");
        const auto err = load_skills(neg, world);
        expect(!err.ok && err.file == "skills.txt",
               "negative skill count rejected with filename");
    }
    {
        wse::platform::MemoryVfs partial;
        expect(partial.write_text("Modules/Mod/info_pages.txt",
                                  "infopagesfile version 1\n1\nip_broken\n"),
               "write corrupt optional info_pages");
        ModuleLoadOptions opts{};
        opts.require_core_files = false;
        const auto err = load_module(partial, "Modules/Mod", ctx, bundle, opts);
        expect(!err.ok && err.file.find("info_pages.txt") != std::string::npos,
               "corrupt optional file still fails loudly even when require_core_files=false");
    }
}

void verify_loaded_execution(wse::platform::Vfs& vfs,
                             std::string_view mod_dir,
                             const char* label) {
    using namespace wse::game;
    wse::vm::VmContext ctx{};
    ModuleBundle bundle{};
    ModuleLoadOptions opts{};
    opts.native_dir = "Modules/Native";
    opts.root_data_dir = "Data";
    const auto err = load_module(vfs, mod_dir, ctx, bundle, opts);
    if (!expect_ok(err, label)) {
        return;
    }
    expect(bundle.loaded_files.size() >= 30U, "bundle tracks loaded files");
    expect(wse::vm::execute_script(bundle.scripts, "startup_probe", ctx),
           "startup_probe runs against loaded bundle");
    expect(ctx.stringRegisters[0] == u"Welcome to Calradia!",
           "str_store_string resolves global strings.txt entry");
    expect(ctx.stringRegisters[1] == u"Quick Report Ready",
           "str_store_string resolves quick_strings.txt entry");
    expect(ctx.stringRegisters[2] == u"Main Hero",
           "str_store_troop_name reads loaded troop");
    expect(ctx.stringRegisters[3] == u"Kingdom of Swadia",
           "str_store_faction_name reads loaded faction");
    expect(ctx.stringRegisters[4] == u"64",
           "str_store_module_setting reads loaded module_config.ini");
    expect(ctx.stringRegisters[5] == u"250",
           "str_store_game_variable reads loaded game_variables.txt");
    expect(ctx.registers[1] == kCampaignLevelXpTable[4] &&
               ctx.registers[2] == 2,
           "native ops read loaded troop XP and item damage type");
    expect(ctx.errorCount == 0U && ctx.unimplementedCount == 0U,
           "loaded startup script executes with zero VM errors");
}

void test_end_to_end_memory_and_host_vfs() {
    // 1. MemoryVfs full bundle + VM execution
    {
        wse::platform::MemoryVfs mem;
        expect(mem.write_text("Modules/Native/game_variables.txt",
                              "hero_bonus = 10\nnative_flag = 1\n"),
               "seed Native fallback in MemoryVfs");
        write_complete_module(mem, "Modules/TestMod");
        verify_loaded_execution(mem, "Modules/TestMod",
                                "load_module full MemoryVfs");
    }

    // 2. HostVfs real filesystem round-trip + cleanup
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path root =
        fs::temp_directory_path(ec) / "wse_module_loaders_host_test";
    fs::remove_all(root, ec);
    fs::create_directories(root / "Modules" / "TestMod" / "Data", ec);
    fs::create_directories(root / "Modules" / "Native", ec);
    expect(!ec, "host test directories created");
    {
        wse::platform::HostVfs host(root);
        expect(host.write_text("Modules/Native/game_variables.txt",
                               "hero_bonus = 10\n"),
               "seed Native fallback in HostVfs");
        write_complete_module(host, "Modules/TestMod");
        verify_loaded_execution(host, "Modules/TestMod",
                                "load_module full HostVfs");
    }
    fs::remove_all(root, ec);
}

static_assert(!std::is_copy_constructible_v<wse::game::ModuleStringStorage> &&
                  !std::is_copy_assignable_v<wse::game::ModuleStringStorage> &&
                  !std::is_copy_constructible_v<wse::game::ModuleBundle>,
              "bundle string views must not be copied away from their backing");
static_assert(std::is_nothrow_move_constructible_v<wse::game::ModuleStringStorage> &&
                  std::is_move_constructible_v<wse::game::ModuleBundle>,
              "bundles stay movable");

void test_reload_resets_module_state_and_storage_moves() {
    using namespace wse::game;

    wse::platform::MemoryVfs vfs;
    expect(vfs.write_text("Modules/A/info_pages.txt",
                          "infopagesfile version 1\n1\nip_a Title_a Text_a\n") &&
               vfs.write_text("Modules/A/game_variables.txt", "foo = 1\n") &&
               vfs.write_text("Modules/A/module_config.ini", "[sec]\nkey = 1\n") &&
               vfs.write_text("Modules/A/scenes.txt",
                              "scenesfile version 1\n1\n"
                              "scn_a a 0 none none 0 0 1 1 -100 0x0 0 0 0\n") &&
               vfs.write_text("Modules/A/meshes.txt",
                              "1\nmesh_a 0 res_a 0 0 0 0 0 0 1 1 1\n") &&
               vfs.write_text("Modules/A/music.txt", "1\ntrack_a.ogg 0 0\n") &&
               vfs.write_text("Modules/A/Data/skyboxes.txt", build_skyboxes_fixture()) &&
               vfs.write_text("Modules/A/strings.txt",
                              "stringsfile version 1\n2\n"
                              "str_long a_string_long_enough_to_live_on_the_heap\n"
                              "str_s hi\n") &&
               vfs.write_text("Modules/B/scenes.txt", "scenesfile version 1\n0\n"),
           "seed reload fixtures");

    auto ctx = std::make_unique<wse::vm::VmContext>();
    auto bundle = std::make_unique<ModuleBundle>();
    ModuleLoadOptions opts{};
    opts.require_core_files = false;
    opts.native_dir.clear();
    opts.root_data_dir.clear();

    ModuleLoadError err = load_module(vfs, "Modules/A", *ctx, *bundle, opts);
    expect(err.ok, "module A loads");
    const auto& world = ctx->world;
    expect(world.info_pages.size() == 1U && world.sites.size() == 1U &&
               world.m6.sites.size() == 1U && world.mesh_names.size() == 1U &&
               world.misc.num_music_tracks == 1 &&
               world.game_variables.size() == 1U &&
               world.module_settings.size() == 1U &&
               world.m5.skybox_fog_colors[0] == 4286611584U,
           "module A populates world state");

    // Moving the storage keeps the views bound to the moved backing strings.
    expect(bundle->string_storage.global_views.size() == 2U,
           "module A binds two global strings");
    ModuleStringStorage moved = std::move(bundle->string_storage);
    bool views_follow = moved.global_views.size() == moved.global_backing.size();
    for (std::size_t i = 0; views_follow && i < moved.global_views.size(); ++i) {
        views_follow = moved.global_views[i].data() == moved.global_backing[i].data() &&
                       moved.global_views[i] == moved.global_backing[i];
    }
    expect(views_follow && moved.global_views.size() == 2U &&
               moved.global_views[1] == u"hi",
           "moved string storage views point at the moved backing");

    err = load_module(vfs, "Modules/B", *ctx, *bundle, opts);
    expect(err.ok, "module B loads over module A");
    expect(world.info_pages.empty() && world.sites.empty() &&
               world.m6.sites.empty() && world.mesh_names.empty() &&
               world.misc.num_music_tracks == 0 &&
               world.game_variables.empty() && world.module_settings.empty() &&
               world.m5.skybox_fog_colors[0] == 0U,
           "reload drops state from files module B does not ship");
    expect(ctx->globalStrings.empty() && ctx->localStrings.empty(),
           "reload rebinds empty string banks");
}

} // namespace

namespace wse::selftest {

int run_module_loaders_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_reload_resets_module_state_and_storage_moves();
    test_ini_and_token_helpers();
    test_individual_world_loaders();
    test_asset_mission_and_data_loaders();
    test_error_diagnostics();
    test_end_to_end_memory_and_host_vfs();

    if (failures == 0) {
        std::printf("module_loaders tests: PASS\n");
        return 0;
    }
    std::printf("module_loaders tests: FAIL (%d)\n", failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_module_loaders_suite(nullptr);
}
#endif
