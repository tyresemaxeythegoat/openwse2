// Native game-world and mission runtime state backing WSE2 VM handlers.
//
// All sizes, offsets, and validators are grounded in official WSE2 v1.1.5.5
// PDB layouts (mb_warband_wse2_x64.pdb) and binary disassembly
// (mb_warband_wse2_x64.exe):
//   mbTroop            size 0x0a28 (2600), +0x0128 m_flags, +0x0140 m_attributes[4],
//                      +0x0150 m_skills[6] (4-bit packed nibbles), +0x0168 m_proficiencies[7],
//                      +0x0188 m_hitPoints, +0x018c m_skillPoints, +0x0190 m_attributePoints,
//                      +0x0194 m_proficiencyPoints, +0x0a08 m_slots.
//   mbItemKind         size 0x0750 (1872), +0x0090 m_id, +0x0120 m_slots,
//                      +0x01c0 m_variations[0..1279], +0x06e8 m_speedRating,
//                      +0x06ec m_missileSpeed, +0x06f0 m_headArmor, +0x06f4 m_bodyArmor,
//                      +0x06f8 m_legArmor, +0x0708 m_thrustDamage, +0x070c m_swingDamage,
//                      +0x0738 m_bloodParticleSystemNos[2], +0x0740 m_bloodColor,
//                      +0x0748 m_skeletonModel.
//   mbItemKindVariation size 0x0050 (80), +0x0000 m_meshName, +0x0048 m_flags.
//   mbSkill            size 0x00e0 (224), +0x0000 m_description, +0x0048 m_name,
//                      +0x0090 m_id, +0x00d8 m_maxLevel, +0x00dc m_flags.
//   mbPartyTemplate    size 0x0128 (296), +0x0110 m_slots.
//   mbFaction          size 0x05c0 (1472), +0x00e0 m_slots.
//   mbQuest            size 0x06b8 (1720), +0x06a0 m_slots.
//   mbSite             size 0x2be0 (11232), +0x2b80 m_slots, +0x0190 m_waterLevel.
//   mbParty            size 0x5810 (22544), +0x52d0 m_slots.
//   mbnetPlayer        size 0x0408 (1032), +0x0000 m_status (2=active), +0x0268 m_slots.
//   mbAgent            size 0x9620 (38432), +0x0030 m_status, +0x00f8 m_slots.
//   mbMissionObject    size 0x0220 (544), +0x01f8 m_slots.
//   mbSceneProp        size 0x0098 (152).
//   rglMesh            size 0x0368 (872), +0x0068 m_materialName, +0x00c0 m_materialColor.
//   rglMetaMesh        size 0x0158 (344), +0x0048 m_lods[120], +0x00c0 m_numLods, +0x0110 m_meshName.
//   mbGame             size 0x17160 (94560), +0x0420 m_mainPartyNo, +0x043c m_paused,
//                      +0x16ed0..+0x16f50 m_editMode*. (DAT_14061f070 sits at +8.)
//   mbMission          size 0x54a50 (346704), +0x98d8 m_teams (teams 0..7 validated by 0x1401e581e,
//                      slots at +0x9900 + team*0x40), +0xa4f4 m_waterLevel,
//                      +0x54a40 m_useObjectsTooltipEnabled[11].

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wse/vm/slots.h"

namespace wse::game {

inline constexpr std::int32_t kMaxSkills = 42;            // 0x2a (0x1401e63c7)
inline constexpr std::int32_t kSkillIronflesh = 36;       // 0x24 (0x140246125)
inline constexpr std::int32_t kMaxAttributes = 4;         // 0x04 (0x1401ebf06)
inline constexpr std::int32_t kMaxProficiencies = 7;      // 0x07 (0x1401ebf6c)
inline constexpr std::int32_t kMaxSlotIndex = 0xfffff;    // 0x1401e55e3..0x1401e58eb
inline constexpr std::int32_t kNumTeams = 8;              // 0..7 (0x1401e581e)
inline constexpr std::int32_t kNumUseTooltipTypes = 11;   // 0x0b (0x1401ea725)
inline constexpr std::uint32_t kSkillPartyFlagBit = 0x10; // bit 4 (0x1401e63e5)

struct WseSkill {
    std::string description{};
    std::string name{};
    std::string id{};
    std::int32_t max_level = 10;
    std::uint32_t flags = 0;
};

struct WseTroop {
    std::uint64_t flags = 0;
    std::array<std::int32_t, kMaxAttributes> attributes{};
    std::array<std::uint32_t, 6> skills{};
    std::array<float, kMaxProficiencies> proficiencies{};
    float hit_points = 35.0f;
    std::int32_t skill_points = 0;
    std::int32_t attribute_points = 0;
    std::int32_t proficiency_points = 0;
    wse::vm::Slots slots{};

    [[nodiscard]] std::int32_t get_skill_raw(std::int32_t skill_no) const noexcept {
        if (skill_no < 0 || skill_no >= kMaxSkills) {
            return 0;
        }
        const auto word = static_cast<std::size_t>(skill_no / 8);
        const auto shift = static_cast<std::uint32_t>((skill_no % 8) * 4);
        return static_cast<std::int32_t>((skills[word] >> shift) & 0xfu);
    }

    void set_skill_raw(std::int32_t skill_no, std::int32_t value) noexcept {
        if (skill_no < 0 || skill_no >= kMaxSkills) {
            return;
        }
        const auto word = static_cast<std::size_t>(skill_no / 8);
        const auto shift = static_cast<std::uint32_t>((skill_no % 8) * 4);
        const std::uint32_t mask = 0xfu << shift;
        skills[word] =
            (skills[word] & ~mask) | ((static_cast<std::uint32_t>(value) & 0xfu) << shift);
    }

    // Shared HP cap in 0x140246037..0x1402460a0 (troop_set_attribute) and
    // 0x14024616d..0x1402461d7 (troop_set_skill):
    // cap = float(str + clamp(ironflesh, 0, skills[36].m_maxLevel) * 2 + 35).
    void cap_hit_points(std::int32_t ironflesh_max_level) noexcept {
        const std::int32_t upper = std::max<std::int32_t>(0, ironflesh_max_level);
        const std::int32_t ironflesh =
            std::clamp(get_skill_raw(kSkillIronflesh), 0, upper);
        const float cap = static_cast<float>(attributes[0] + ironflesh * 2 + 35);
        if (hit_points > cap) {
            hit_points = cap;
        }
    }

    // Mirrors 0x140246010 (troop_set_attribute).
    void set_attribute(std::int32_t attr_no,
                       std::int32_t value,
                       std::int32_t ironflesh_max_level) noexcept {
        if (attr_no < 0 || attr_no >= kMaxAttributes) {
            return;
        }
        const std::int32_t clamped =
            std::clamp(value, std::int32_t{0}, std::numeric_limits<std::int32_t>::max());
        attributes[static_cast<std::size_t>(attr_no)] = clamped;
        cap_hit_points(ironflesh_max_level);
    }

    // Mirrors 0x1402460b0 (troop_set_skill).
    void set_skill(std::int32_t skill_no,
                   std::int32_t value,
                   std::int32_t skill_max_level,
                   std::int32_t ironflesh_max_level) noexcept {
        if (skill_no < 0 || skill_no >= kMaxSkills) {
            return;
        }
        const std::int32_t upper = std::clamp(skill_max_level, 0, 15);
        const std::int32_t next = std::clamp(value, 0, upper);
        const std::int32_t prev = std::clamp(get_skill_raw(skill_no), 0, upper);
        if (skill_no == kSkillIronflesh && next > prev) {
            hit_points += static_cast<float>((next - prev) * 2);
        }
        set_skill_raw(skill_no, next);
        cap_hit_points(ironflesh_max_level);
    }

    // Mirrors 0x1401ebf45..0x1401ebf9a via 0x1400a61c0(value, 0, 1000) -> [0, 999].
    void set_proficiency(std::int32_t prof_no, std::int32_t value) noexcept {
        if (prof_no < 0 || prof_no >= kMaxProficiencies) {
            return;
        }
        proficiencies[static_cast<std::size_t>(prof_no)] =
            static_cast<float>(std::clamp(value, 0, 999));
    }
};

struct WseMesh {
    std::string name{};
    std::string material_name{};
    std::uint32_t material_color = 0xffffffffu;
    std::vector<std::uint32_t> vertex_colors{};
    std::uint32_t vertex_color_override = 0;
    bool has_vertex_color_override = false;

    // Mirrors 0x1402c1af0: rewrites per-vertex color (+0x38 of stride 0x44).
    void set_color(std::uint32_t color) noexcept {
        std::fill(vertex_colors.begin(), vertex_colors.end(), color);
        vertex_color_override = color;
        has_vertex_color_override = true;
    }

    // Mirrors 0x1402ba5c0: assigns mesh material.
    void set_material(std::string_view material) {
        material_name = std::string(material);
    }
};

struct WseMetaMesh {
    static constexpr std::size_t kMaxLods = 120;

    std::string mesh_name{};
    std::vector<std::vector<WseMesh>> lods{};

    // Mirrors 3811 cur_item_mesh_set_color (0x1401ec2c0): applies color across
    // all active LODs at index mesh_no if within that LOD's mesh count.
    void set_mesh_color(std::int32_t mesh_no, std::uint32_t color) noexcept {
        if (mesh_no < 0) {
            return;
        }
        const auto idx = static_cast<std::size_t>(mesh_no);
        for (auto& lod : lods) {
            if (idx < lod.size()) {
                lod[idx].set_color(color);
            }
        }
    }
};

struct WseItemKindVariation {
    std::string mesh_name{};
    std::uint64_t flags = 0;
};

struct WseItemKind {
    std::string plural_name{};
    std::string name{};
    std::string id{};
    wse::vm::Slots slots{};
    std::vector<WseItemKindVariation> variations{WseItemKindVariation{}};
    std::int32_t speed_rating = 0;
    std::int32_t missile_speed = 0;
    std::int32_t head_armor = 0;
    std::int32_t body_armor = 0;
    std::int32_t leg_armor = 0;
    std::int32_t thrust_damage = 0;
    std::int32_t swing_damage = 0;
    std::array<std::int32_t, 2> blood_particle_system_nos{-1, -1};
    std::uint32_t blood_color = 0;
    std::string skeleton_model{};
};

struct WsePartyTemplate {
    std::string name{};
    std::string id{};
    wse::vm::Slots slots{};
};

struct WseFaction {
    std::string name{};
    std::string id{};
    wse::vm::Slots slots{};
};

struct WseQuest {
    std::string id{};
    wse::vm::Slots slots{};
};

struct WseSite {
    std::string id{};
    std::string name{};
    float water_level = 0.0f;
    wse::vm::Slots slots{};
};

struct WseParty {
    std::string name{};
    std::string id{};
    bool active = true;
    wse::vm::Slots slots{};
};

struct WsePlayer {
    std::int32_t status = 2; // mbnetPlayer::m_status == 2 checked by 0x1401c5530
    std::string name{};
    wse::vm::Slots slots{};
};

struct WseTeam {
    wse::vm::Slots slots{};
};

struct WseAgent {
    bool active = true;
    std::int32_t status = 0;
    wse::vm::Slots slots{};
};

struct WseMissionObject {
    bool active = true;
    wse::vm::Slots slots{};
};

struct WseEditModeState {
    bool active = false;                                  // mbGame +0x16ed0
    std::int32_t mode = 0;                                // mbGame +0x16ed4
    bool dialog_open = false;                             // mbGame +0x16f30
    std::int32_t highlighted_index = -1;                  // mbGame +0x16f34
    std::vector<std::int32_t> selected_prop_instances{};  // mbGame +0x16f38
    bool selection_changed = false;                       // mbGame +0x16f50
};

struct WseWorldState {
    bool has_game = true;
    std::int32_t main_party_no = 0;                       // mbGame +0x0420
    std::int64_t campaign_time = 0;                       // DAT_14061f038
    bool paused = false;                                  // mbGame +0x043c
    bool hide_hud = false;                                // DAT_140657110
    float water_level = 0.0f;                             // mbMission +0xa4f4
    std::array<bool, kNumUseTooltipTypes> use_objects_tooltip_enabled{};
    WseEditModeState edit_mode{};

    std::array<WseSkill, kMaxSkills> skills{};            // 0x14065f810
    std::vector<WseItemKind> items{};
    std::vector<WsePartyTemplate> party_templates{};
    std::vector<WseTroop> troops{};
    std::vector<WseFaction> factions{};
    std::vector<WseQuest> quests{};
    std::vector<WseSite> sites{};
    std::vector<WseParty> parties{};
    std::vector<WsePlayer> players{};
    std::array<WseTeam, kNumTeams> teams{};
    std::vector<WseAgent> agents{};
    std::vector<WseMissionObject> mission_objects{};
    std::int32_t num_particle_systems = 0;                // DAT_140623a10

    WseMetaMesh* cur_item_meta_mesh = nullptr;            // 0x1401c5e40 (caller -50)
    std::unordered_map<std::string, std::vector<WseMesh>> resource_meshes{};
    std::unordered_map<std::string, bool> resource_materials{};
    std::unordered_map<std::string, bool> resource_skeleton_models{};

    // Case-sensitive INI/variable/localization stores (0x1402a7da0, 0x1402aa400, 0x14065de80).
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        module_settings{};
    std::unordered_map<std::string, std::string> game_variables{};
    std::unordered_map<std::string, std::string> translations{};
    std::filesystem::path savegames_dir{};

    [[nodiscard]] bool is_valid_item(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < items.size();
    }
    [[nodiscard]] bool is_valid_party_template(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < party_templates.size();
    }
    [[nodiscard]] bool is_valid_troop(std::int32_t id) const noexcept {
        return has_game && id >= 0 && static_cast<std::size_t>(id) < troops.size();
    }
    [[nodiscard]] bool is_valid_faction(std::int32_t id) const noexcept {
        return has_game && id >= 0 && static_cast<std::size_t>(id) < factions.size();
    }
    [[nodiscard]] bool is_valid_quest(std::int32_t id) const noexcept {
        return has_game && id >= 0 && static_cast<std::size_t>(id) < quests.size();
    }
    [[nodiscard]] bool is_valid_site(std::int32_t id) const noexcept {
        return has_game && id >= 0 && static_cast<std::size_t>(id) < sites.size();
    }
    [[nodiscard]] bool is_valid_party(std::int32_t id) const noexcept {
        return has_game && id >= 0 && static_cast<std::size_t>(id) < parties.size() &&
               parties[static_cast<std::size_t>(id)].active;
    }
    [[nodiscard]] bool is_valid_player(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < players.size() &&
               players[static_cast<std::size_t>(id)].status == 2;
    }
    [[nodiscard]] bool is_valid_team(std::int32_t id) const noexcept {
        return id >= 0 && id < kNumTeams;
    }
    [[nodiscard]] bool is_valid_agent(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < agents.size() &&
               agents[static_cast<std::size_t>(id)].active;
    }
    [[nodiscard]] bool is_valid_mission_object(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < mission_objects.size() &&
               mission_objects[static_cast<std::size_t>(id)].active;
    }
    [[nodiscard]] bool is_valid_particle_system(std::int32_t id) const noexcept {
        return id >= 0 && id < num_particle_systems;
    }
};

}  // namespace wse::game
