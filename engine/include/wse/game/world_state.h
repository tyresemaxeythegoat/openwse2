// Native game-world and mission runtime state backing WSE2 VM handlers.
//
// All sizes, offsets, and validators are grounded in official WSE2 v1.1.5.6
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
//                      +0x16ed0..+0x16f50 m_editMode*. (DAT_140620070 sits at +8.)
//   mbMission          size 0x54a50 (346704), +0x98d8 m_teams (teams 0..7 validated by 0x1401e599e,
//                      slots at +0x9900 + team*0x40), +0xa4f4 m_waterLevel,
//                      +0x54a40 m_useObjectsTooltipEnabled[11].

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "wse/core/math.h"
#include "wse/game/audio_runtime.h"
#include "wse/game/world_subsystem_types.h"
#include "wse/game/world_m6_types.h"
#include "wse/platform/vfs.h"
#include "wse/vm/slots.h"

namespace wse::game {

inline constexpr std::int32_t kMaxSkills = 42;            // 0x2a (0x1401e6547)
inline constexpr std::int32_t kSkillInventoryManagement = 12; // 0x0c (0x140246810)
inline constexpr std::int32_t kSkillRiding = 24;          // 0x18 (0x14010fdc0)
inline constexpr std::int32_t kSkillShield = 26;          // 0x1a (0x14010fdc0)
inline constexpr std::int32_t kSkillPowerDraw = 33;       // 0x21 (0x14010fdc0)
inline constexpr std::int32_t kSkillPowerThrow = 34;      // 0x22 (0x14010fdc0)
inline constexpr std::int32_t kSkillIronflesh = 36;       // 0x24 (0x1402462a5)
inline constexpr std::int32_t kNumItemModifiers = 43;     // 0x2b (0x140093f80)
inline constexpr std::int32_t kMaxAttributes = 4;         // 0x04 (0x1401ec086)
inline constexpr std::int32_t kMaxProficiencies = 7;      // 0x07 (0x1401ec0ec)
inline constexpr std::int32_t kMaxSlotIndex = 0xfffff;    // 0x1401e5763..0x1401e5a6b
inline constexpr std::int32_t kNumTeams = 8;              // 0..7 (0x1401e599e)
inline constexpr std::int32_t kNumUseTooltipTypes = 11;   // 0x0b (0x1401ea8a5)
inline constexpr std::uint32_t kSkillPartyFlagBit = 0x10; // bit 4 (0x1401e6565)
inline constexpr std::int32_t kMaxPlayers = 250;          // 0xfa (0x1401c7fcc)
inline constexpr std::int32_t kMaxEntryPoints = 128;      // 0x80 (0x1401d5b71)
inline constexpr std::int32_t kNumTroopEquipmentSlots = 10;     // 0..9 (0x1401d0ce4)
inline constexpr std::int32_t kNumTroopInventorySlots = 96;     // 0x60 (0x1402453d0)
inline constexpr std::int32_t kTotalTroopInventoryCapacity =
    kNumTroopEquipmentSlots + kNumTroopInventorySlots;
inline constexpr std::int32_t kMaxAgentItems = 4;         // 0..3 (0x1401d6a00)
inline constexpr std::int32_t kNumQuestNotes = 16;        // 0..15 (0x1401cdf69)
inline constexpr std::int32_t kNumDivisions = 9;          // 0..8 (0x1401d6178)
// Safety deviation (no retail limit): party script IDs index the dense `parties` vector, so a
// removed party's key stays allocated as a tombstone and every key below `next_party_key` costs
// one `WseParty`. Loaders reject, and `allocate_party` refuses, keys at or above this cap so a
// save or `parties.txt` cannot force a multi-gigabyte allocation through `m_nextKey`.
inline constexpr std::int32_t kMaxPartyKeys = 1 << 17;

struct WseSkill {
    std::string description{};
    std::string name{};
    std::string id{};
    std::int32_t max_level = 10;
    std::uint32_t flags = 0;
};

// Runtime subset of Data/item_modifiers.txt (43 entries, 0x140093f80) read by
// mbTroop::sortInventoryByValue (0x140247900), mbTroopInventory::compactSort (0x140245430),
// and mbGame::addMerchandiseToTroop (0x1400e2520).
struct WseItemModifierStats {
    float price_multiplier = 1.0f;
    float rarity_multiplier = 1.0f;
};

struct WseInventorySlot {
    std::int32_t item_no = -1;
    std::int32_t modifier = 0;
    std::int32_t ammo = 0;
    std::int32_t hit_points = 0;

    [[nodiscard]] constexpr bool is_empty() const noexcept {
        return item_no < 0;
    }

    void clear() noexcept {
        item_no = -1;
        modifier = 0;
        ammo = 0;
        hit_points = 0;
    }
};

struct WsePartyStack {
    std::int32_t troop_no = -1;
    std::int32_t num_troops = 0;
    std::int32_t num_wounded_troops = 0;
    std::uint32_t flags = 0;  // bit 0 (0x1): prisoner stack
    std::int32_t dna = 0;
    std::int32_t num_upgradeable = 0;
    float xp = 0.0f;

    [[nodiscard]] constexpr bool is_prisoner() const noexcept {
        return (flags & 1U) != 0U;
    }

    [[nodiscard]] constexpr std::int32_t unwounded() const noexcept {
        return num_troops > num_wounded_troops ? (num_troops - num_wounded_troops) : 0;
    }
};

// Safety deviation: script-supplied counts are added with saturation instead of
// wrapping, so a hostile or buggy module cannot trigger signed overflow.
[[nodiscard]] constexpr std::int32_t saturating_add_i32(std::int32_t a,
                                                        std::int32_t b) noexcept {
    const std::int64_t sum = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    return static_cast<std::int32_t>(
        std::clamp<std::int64_t>(sum, std::numeric_limits<std::int32_t>::min(),
                                 std::numeric_limits<std::int32_t>::max()));
}

// Safety deviation: upper bound on one site's visitor list, far above any
// mission agent limit, so add_visitors_to_current_scene cannot exhaust memory.
inline constexpr std::size_t kMaxSiteVisitors = 4096;

struct WseSiteVisitor {
    std::int32_t entry_no = 0;
    std::int32_t troop_no = -1;
    std::int32_t dna = -0xd0cd;
};

struct WseQuestNote {
    std::u16string text{};
    std::int32_t date = -1;
};

struct WsePresentationOverlay {
    std::int32_t type = 0;
    std::u16string text{};
    std::uint32_t color = 0xffffffffu;
    float alpha = 1.0f;
    wse::core::WseVec2 position{};
    wse::core::WseVec2 size{1.0f, 1.0f};
    std::int32_t val = 0;
    bool visible = true;
    WseOverlayM5Ext m5{};
};

struct WseMissionTemplate {
    struct EntryOverride {
        std::uint32_t flags = 0;
        std::vector<std::int32_t> override_items{};
    };

    std::string id{};
    std::string name{};
    std::array<EntryOverride, kMaxEntryPoints> entry_overrides{};
    bool all_agents_spawned = true;
};

struct WseMultiplayerMessage {
    std::int32_t player_no = -1;
    std::int32_t event_type = 0;
    std::array<std::int64_t, 4> ints{};
    std::int32_t num_ints = 0;
    std::u16string text{};
};

struct WseBattleEvent {
    std::int32_t type = 0;
    std::int32_t sound_no = -1;
    std::int32_t agent_no = -1;
    std::int32_t flags = 0;
    wse::core::WseVec4 position{};
    float simulation_time = 0.0f;
};

struct WseEntryPoint {
    bool valid = false;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    std::int32_t num_x_moves = 0;
    std::int32_t num_y_moves = 0;
    bool is_auto_generated = false;
};

// Mirrors rglTimer (0x1403197a0): elapsed seconds = (timersTotal[timer_no] - value) / 100000.0f.
struct WseMissionTimer {
    std::int32_t timer_no = 0;
    std::int64_t value = 0;

    [[nodiscard]] float elapsed_seconds(
        const std::array<std::int64_t, 3>& totals) const noexcept {
        if (timer_no < 0 || timer_no >= 3) {
            return 0.0f;
        }
        return static_cast<float>(totals[static_cast<std::size_t>(timer_no)] - value) /
               100000.0f;
    }
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
    bool renamed = false;       // mbTroop +0x0 m_renamed
    std::string name{};
    std::string plural_name{};
    std::int32_t faction_no = 0;  // mbTroop::m_factionNo (0x1401ddb58)
    std::int32_t gold = 0;        // mbTroop::m_gold (0x1401dd733)
    std::int32_t skin_no = 0;     // mbTroop::m_skinNo (0x140245ba0)
    std::int32_t class_no = 0;    // mbTroop::m_classNo (0x1401d05bd)
    std::int32_t level = 1;       // mbTroop::m_level (0x1401ddaab)
    bool auto_equip = false;      // mbTroop::m_flags bit / auto-equip (0x1401d0450)
    std::array<WseInventorySlot, kTotalTroopInventoryCapacity> inventory{};
    std::int32_t xp = 0;
    std::array<std::int32_t, 2> upgrade_troop_nos{-1, -1};
    std::int32_t home_site_no = -1;
    std::int32_t home_entry_no = 0;
    float age = 0.0f;
    std::array<std::array<std::uint64_t, 4>, 2> face_keys{};
    bool note_available = true;
    std::int32_t note_tableau_material_no = -1;
    std::array<WseQuestNote, kNumQuestNotes> notes{};

    void set_age(std::int32_t value) noexcept {
        const std::int32_t clamped = std::clamp(value, std::int32_t{0}, std::int32_t{99});
        age = static_cast<float>(clamped) * 0.01f;
    }

    [[nodiscard]] std::string face_keys_to_string(std::int32_t slot) const {
        if (slot < 0 || slot >= 2) {
            return std::string(64, '0');
        }
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out(64, '0');
        const auto& words = face_keys[static_cast<std::size_t>(slot)];
        for (std::size_t w = 0; w < 4; ++w) {
            std::uint64_t v = words[w];
            for (std::size_t d = 0; d < 16; ++d) {
                out[w * 16 + (15 - d)] = kHex[v & 0xfULL];
                v >>= 4U;
            }
        }
        return out;
    }

    void face_keys_from_string(std::int32_t slot, std::string_view hex) noexcept {
        if (slot < 0 || slot >= 2) {
            return;
        }
        auto hex_val = [](char ch) noexcept -> std::uint64_t {
            if (ch >= '0' && ch <= '9') {
                return static_cast<std::uint64_t>(ch - '0');
            }
            if (ch >= 'a' && ch <= 'f') {
                return static_cast<std::uint64_t>(10 + (ch - 'a'));
            }
            if (ch >= 'A' && ch <= 'F') {
                return static_cast<std::uint64_t>(10 + (ch - 'A'));
            }
            return 0ULL;
        };
        auto& words = face_keys[static_cast<std::size_t>(slot)];
        for (std::size_t w = 0; w < 4; ++w) {
            std::uint64_t acc = 0ULL;
            for (std::size_t d = 0; d < 16; ++d) {
                const std::size_t idx = w * 16 + d;
                acc = (acc << 4U) | (idx < hex.size() ? hex_val(hex[idx]) : 0ULL);
            }
            words[w] = acc;
        }
    }

    // Mirrors mbTroop::isHero (0x1402460e0): (m_flags >> 4) & 1.
    [[nodiscard]] constexpr bool is_hero() const noexcept {
        return ((flags >> 4U) & 1U) != 0U;
    }

    // Mirrors mbTroop::getMaxHitPoints (0x140245c20):
    // m_attributes[0] + 35 + min(ironflesh, g_skills[36].m_maxLevel) * 2.
    [[nodiscard]] std::int32_t max_hit_points(
        std::int32_t ironflesh_max_level) const noexcept {
        const std::int32_t upper = std::max<std::int32_t>(0, ironflesh_max_level);
        const std::int32_t ironflesh =
            std::clamp(get_skill_raw(kSkillIronflesh), 0, upper);
        return attributes[0] + 35 + ironflesh * 2;
    }

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

    // Shared HP cap in 0x1402461b7..0x140246220 (troop_set_attribute) and
    // 0x1402462ed..0x140246357 (troop_set_skill):
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

    // Mirrors 0x140246190 (troop_set_attribute).
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

    // Mirrors 0x140246230 (troop_set_skill).
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

    // Mirrors 0x1401ec0c5..0x1401ec11a via 0x1400a6320(value, 0, 1000) -> [0, 999].
    void set_proficiency(std::int32_t prof_no, std::int32_t value) noexcept {
        if (prof_no < 0 || prof_no >= kMaxProficiencies) {
            return;
        }
        proficiencies[static_cast<std::size_t>(prof_no)] =
            static_cast<float>(std::clamp(value, 0, 999));
    }

    // Mirrors mbTroop::getSkinNo (0x140245ba0).
    [[nodiscard]] constexpr std::int32_t get_skin_no() const noexcept {
        return skin_no;
    }

    // Mirrors mbTroop::setSkinNo (0x1402460b0): writes skin_no and low 4 bits of flags.
    void set_skin_no(std::int32_t new_skin) noexcept {
        skin_no = new_skin;
        flags = (flags & ~std::uint64_t{0xf}) | (static_cast<std::uint64_t>(new_skin) & 0xfU);
    }

    // Mirrors mbTroop::getNumInventorySlots (0x140246810):
    // hero uses min(inv_mgmt, max_level) -> min(9, lvl)*6 + 30 or 96 for >= 10; non-hero -> 90.
    [[nodiscard]] std::int32_t get_num_inventory_slots(
        std::int32_t inv_mgmt_max_level) const noexcept {
        if (!is_hero()) {
            return 90;
        }
        const std::int32_t cap = std::max<std::int32_t>(0, inv_mgmt_max_level);
        const std::int32_t lvl =
            std::clamp(get_skill_raw(kSkillInventoryManagement), 0, cap);
        if (lvl >= 10) {
            return kNumTroopInventorySlots;
        }
        return 30 + lvl * 6;
    }

    // Mirrors mbTroop::getFirstFreeInventorySlot (0x1402468d0), offset by kNumTroopEquipmentSlots (10)
    // to index the combined 10+96 array used by troop_get_inventory_slot.
    [[nodiscard]] std::int32_t get_first_free_inventory_slot(
        std::int32_t inv_mgmt_max_level) const noexcept {
        const std::int32_t bag_slots = get_num_inventory_slots(inv_mgmt_max_level);
        for (std::int32_t i = 0; i < bag_slots; ++i) {
            const auto idx = static_cast<std::size_t>(kNumTroopEquipmentSlots + i);
            if (idx < inventory.size() && inventory[idx].is_empty()) {
                return kNumTroopEquipmentSlots + i;
            }
        }
        return -1;
    }

    // Mirrors mbTroop::setItem (0x140246920).
    void set_item(std::int32_t slot_no,
                  std::int32_t item_no,
                  std::int32_t modifier = 0,
                  std::int32_t max_ammo = 0) noexcept {
        if (slot_no < 0 || static_cast<std::size_t>(slot_no) >= inventory.size()) {
            return;
        }
        auto& slot = inventory[static_cast<std::size_t>(slot_no)];
        slot.item_no = item_no;
        slot.modifier = (modifier >= 0 && modifier < 0x2b) ? modifier : 0;
        slot.ammo = item_no >= 0 ? max_ammo : 0;
        slot.hit_points = 0;
    }

    // Mirrors mbTroopInventory::clear (0x1402453d0): clears bag slots 10..105.
    void clear_inventory() noexcept {
        for (std::size_t i = static_cast<std::size_t>(kNumTroopEquipmentSlots);
             i < inventory.size(); ++i) {
            inventory[i].clear();
        }
    }

    // Mirrors mbTroop::raiseAttribute (0x140246370).
    void raise_attribute(std::int32_t attr_no,
                         std::int32_t delta,
                         std::int32_t ironflesh_max_level,
                         std::int32_t attr_limit) noexcept {
        if (attr_no < 0 || attr_no >= kMaxAttributes || delta == 0) {
            return;
        }
        const std::int32_t step = delta < 0 ? -1 : 1;
        std::int32_t rem = delta < 0 ? -delta : delta;
        while (rem > 0) {
            --rem;
            const std::int32_t next = attributes[static_cast<std::size_t>(attr_no)] + step;
            if (next >= 0 && next < attr_limit) {
                if (attr_no == 2) {
                    skill_points += step;
                } else if (attr_no == 0) {
                    hit_points += static_cast<float>(step);
                }
                set_attribute(attr_no, next, ironflesh_max_level);
            }
        }
    }

    // Mirrors mbTroop::raiseSkill (0x140246430).
    void raise_skill(std::int32_t skill_no,
                     std::int32_t delta,
                     std::int32_t skill_max_level,
                     std::int32_t ironflesh_max_level) noexcept {
        if (skill_no < 0 || skill_no >= kMaxSkills) {
            return;
        }
        const std::int32_t cap = std::max<std::int32_t>(0, skill_max_level);
        const std::int32_t cur = std::min(get_skill_raw(skill_no), cap);
        set_skill(skill_no, cur + delta, skill_max_level, ironflesh_max_level);
    }

    // Mirrors mbTroop::raiseProficiencyLinear (0x1402465d0).
    void raise_proficiency_linear(std::int32_t prof_no, std::int32_t delta) noexcept {
        if (prof_no < 0 || prof_no >= kMaxProficiencies) {
            return;
        }
        const float next =
            proficiencies[static_cast<std::size_t>(prof_no)] + static_cast<float>(delta);
        proficiencies[static_cast<std::size_t>(prof_no)] = std::clamp(next, 0.0f, 1000.0f);
    }
};

struct WseMesh {
    std::string name{};
    std::string material_name{};
    std::uint32_t material_color = 0xffffffffu;
    std::vector<std::uint32_t> vertex_colors{};
    std::uint32_t vertex_color_override = 0;
    bool has_vertex_color_override = false;

    // Mirrors 0x1402c1d60: rewrites per-vertex color (+0x38 of stride 0x44).
    void set_color(std::uint32_t color) noexcept {
        std::fill(vertex_colors.begin(), vertex_colors.end(), color);
        vertex_color_override = color;
        has_vertex_color_override = true;
    }

    // Mirrors 0x1402ba830: assigns mesh material.
    void set_material(std::string_view material) {
        material_name = std::string(material);
    }
};

struct WseMetaMesh {
    static constexpr std::size_t kMaxLods = 120;

    std::string mesh_name{};
    std::vector<std::vector<WseMesh>> lods{};

    // Mirrors 3811 cur_item_mesh_set_color (0x1401ec440): applies color across
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
    std::uint64_t properties = 0;
    std::uint64_t modifiers = 0;
    std::vector<std::int32_t> factions{};
    std::int32_t max_ammo = 0;
    std::int32_t value = 0;
    std::uint64_t capabilities = 0;
    std::int32_t difficulty = 0;
    std::int32_t hit_points = 0;
    float weight = 0.0f;
    std::int32_t weapon_length = 0;
    std::int32_t accuracy = 0;
    float shield_height = 0.0f;
    float horse_scale = 0.0f;
    std::int32_t horse_speed = 0;
    std::int32_t horse_maneuver = 0;
    std::int32_t food_quality = 0;
    std::int32_t abundance = 100;
    std::int32_t thrust_damage_type = 0;
    std::int32_t swing_damage_type = 0;
    std::int32_t horse_charge_damage = 0;
    float price_rate = 1.0f;
    float probability = 100.0f;

    [[nodiscard]] constexpr std::int32_t get_type() const noexcept {
        return static_cast<std::int32_t>(properties & 0xffULL);
    }
    [[nodiscard]] constexpr bool is_armor() const noexcept {
        const std::int32_t t = get_type();
        return t >= 12 && t <= 15;
    }
    [[nodiscard]] constexpr std::int32_t get_thrust_damage() const noexcept {
        return thrust_damage & 0xff;
    }
    [[nodiscard]] constexpr std::int32_t get_thrust_damage_type() const noexcept {
        return thrust_damage_type != 0 ? thrust_damage_type : ((thrust_damage >> 8) & 3);
    }
    [[nodiscard]] constexpr std::int32_t get_swing_damage() const noexcept {
        return swing_damage & 0xff;
    }
    [[nodiscard]] constexpr std::int32_t get_swing_damage_type() const noexcept {
        return swing_damage_type != 0 ? swing_damage_type : ((swing_damage >> 8) & 3);
    }
    [[nodiscard]] constexpr std::int32_t get_horse_charge() const noexcept {
        return horse_charge_damage != 0 ? horse_charge_damage : (thrust_damage & 0xff);
    }
    [[nodiscard]] constexpr std::int32_t get_accuracy() const noexcept {
        return accuracy != 0 ? accuracy : leg_armor;
    }
    [[nodiscard]] constexpr std::int32_t get_horse_speed() const noexcept {
        return horse_speed != 0 ? horse_speed : missile_speed;
    }
    [[nodiscard]] constexpr std::int32_t get_horse_maneuver() const noexcept {
        return horse_maneuver != 0 ? horse_maneuver : speed_rating;
    }
    [[nodiscard]] constexpr std::int32_t get_food_quality() const noexcept {
        return food_quality != 0 ? food_quality : head_armor;
    }
    [[nodiscard]] constexpr float get_shield_height() const noexcept {
        return shield_height > 0.0f ? shield_height : static_cast<float>(missile_speed) / 100.0f;
    }
    [[nodiscard]] constexpr float get_horse_scale() const noexcept {
        if (horse_scale > 0.0f) {
            return horse_scale;
        }
        return weapon_length == 0 ? 1.0f : static_cast<float>(weapon_length) / 100.0f;
    }
};

struct WsePartyTemplate {
    struct StackTemplate {
        std::int32_t troop_no = -1;
        std::int32_t min_troops = 0;
        std::int32_t max_troops = 0;
        std::uint32_t flags = 0;
    };

    std::string name{};
    std::string id{};
    wse::vm::Slots slots{};
    std::uint64_t flags = 0;
    std::int32_t icon_no = 0;
    std::int32_t faction_no = 0;
    std::int32_t menu_no = 0;
    std::uint32_t personality = 0;
    std::array<StackTemplate, 6> stacks{};
    std::int32_t num_created = 0;
    std::int32_t num_destroyed = 0;
    std::int32_t num_destroyed_by_player = 0;
};

struct WseFaction {
    std::string name{};
    std::string id{};
    wse::vm::Slots slots{};
    bool renamed = false;       // mbFaction +0x48 m_renamed
    std::uint32_t color = 0xffffffffu;
    std::vector<float> relations{};
    bool note_available = true;
    std::int32_t note_tableau_material_no = -1;
    std::array<WseQuestNote, kNumQuestNotes> notes{};

    void set_relation(std::size_t other_idx, float relation) {
        if (relations.size() <= other_idx) {
            relations.resize(other_idx + 1, 0.0f);
        }
        relations[other_idx] = relation;
    }

    [[nodiscard]] float get_relation(std::size_t other_idx) const noexcept {
        return other_idx < relations.size() ? relations[other_idx] : 0.0f;
    }
};

struct WseQuest {
    std::string id{};
    wse::vm::Slots slots{};
    std::string title{};                 // mbQuest::m_title (0x1401dee13 / 0x1401df144)
    std::uint32_t progression_flags = 0; // bit8 active, bit9 completed, bit10 succeeded, bit11 failed, bit12 concluded
    std::uint32_t template_flags = 0;
    std::int32_t giver_troop_no = -1;
    float start_date = 0.0f;
    bool note_available = true;
    std::int32_t note_tableau_material_no = -1;
    std::array<WseQuestNote, kNumQuestNotes> notes{};
    std::string text{};
    std::string giver_name{};   // mbQuest +0x90 m_giverName
    std::int32_t number = 0;
    std::int32_t item_no = -1;
    std::int32_t troop_no = -1;
    std::int32_t target_party_no = -1;
    std::int32_t target_site_no = -1;
    std::int32_t target_faction_no = -1;
    float expiration_days = 0.0f;

    [[nodiscard]] constexpr bool is_active() const noexcept { return ((progression_flags >> 8U) & 1U) != 0U; }
    [[nodiscard]] constexpr bool is_completed() const noexcept { return ((progression_flags >> 9U) & 1U) != 0U; }
    [[nodiscard]] constexpr bool is_succeeded() const noexcept { return ((progression_flags >> 10U) & 1U) != 0U; }
    [[nodiscard]] constexpr bool is_failed() const noexcept { return ((progression_flags >> 11U) & 1U) != 0U; }
    [[nodiscard]] constexpr bool is_concluded() const noexcept { return ((progression_flags >> 12U) & 1U) != 0U; }
};

struct WseSite {
    std::string id{};
    std::string name{};
    float water_level = 0.0f;
    wse::vm::Slots slots{};
    std::vector<WseSiteVisitor> visitors{};
    std::uint32_t flags = 0;
};

struct WseParty {
    std::string name{};
    std::string id{};
    bool active = true;
    bool renamed = false;       // mbParty +0x58 m_renamed
    wse::vm::Slots slots{};
    std::int32_t faction_no = 0;  // mbParty::m_factionNo (0x1401ddeff)
    std::uint64_t flags = 0;
    std::int32_t icon_no = 0;
    std::int32_t banner_icon_no = -1;
    std::int32_t menu_no = 0;     // mbParty +0x138 m_menuNo
    float bearing = 0.0f;         // mbParty +0xc0 m_bearing
    std::string extra_text{};
    std::int32_t behavior = 0;
    std::int32_t behavior_object = -1;
    wse::core::WseVec2 behavior_position{};  // mbParty +0x208 m_behaviorPosition
    std::int32_t ai_state = 0;
    std::int32_t attached_to_party_no = -1;
    std::int32_t battle_opponent_no = -1;
    std::int32_t current_terrain = 0;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    std::vector<WsePartyStack> stacks{};
    std::array<std::int32_t, kMaxSkills> skill_levels{};
    std::int32_t template_no = -1;
    std::int32_t extra_icon_no = -1;
    std::int32_t player_no = -1;
    std::int32_t controller = 1;
    std::int32_t terrain_variant = 0;
    float morale = 0.5f;
    bool is_marshall = false;
    std::uint32_t personality = 0;
    bool ignore_with_player_party = false;
    std::int32_t bandit_attraction = 0;
    float radius = 0.25f;
    std::vector<std::int32_t> attached_party_nos{};
    std::int32_t default_behavior = 0;
    std::int32_t default_behavior_object = -1;
    wse::core::WseVec2 default_behavior_position{};
    float patrol_radius = 0.0f;
    float ai_initiative = 1.0f;
    float helpfulness = 1.0f;
    float ignore_player_timer = 0.0f;
    bool in_battle = false;
    bool note_available = true;
    std::int32_t note_tableau_material_no = -1;
    std::array<WseQuestNote, kNumQuestNotes> notes{};
    std::vector<std::int32_t> particle_system_nos{};
    float extra_icon_up_down_distance = 0.0f;
    float extra_icon_up_down_frequency = 0.0f;
    float extra_icon_rotate_frequency = 0.0f;
    float extra_icon_fade_distance = 0.0f;
    std::int32_t battle_event_index = -1;

    void set_aggressiveness(std::int32_t value) noexcept {
        const std::uint32_t clamped =
            static_cast<std::uint32_t>(std::clamp(value, std::int32_t{0}, std::int32_t{15}));
        personality = (personality & 0xffffff0fU) | (clamped << 4U);
    }

    [[nodiscard]] constexpr std::int32_t get_aggressiveness() const noexcept {
        return static_cast<std::int32_t>((personality >> 4U) & 0xfU);
    }

    void set_courage(std::int32_t value) noexcept {
        const std::uint32_t clamped =
            static_cast<std::uint32_t>(std::clamp(value, std::int32_t{0}, std::int32_t{15}));
        personality = (personality & 0xfffffff0U) | clamped;
    }

    [[nodiscard]] constexpr std::int32_t get_courage() const noexcept {
        return static_cast<std::int32_t>(personality & 0xfU);
    }

    [[nodiscard]] constexpr bool is_bot() const noexcept {
        return controller == 1;
    }

    [[nodiscard]] std::int32_t get_num_companions() const noexcept {
        std::int32_t total = 0;
        for (const auto& s : stacks) {
            if (!s.is_prisoner()) {
                total += s.num_troops;
            }
        }
        return total;
    }

    [[nodiscard]] std::int32_t get_num_prisoners() const noexcept {
        std::int32_t total = 0;
        for (const auto& s : stacks) {
            if (s.is_prisoner()) {
                total += s.num_troops;
            }
        }
        return total;
    }

    [[nodiscard]] std::int32_t get_size() const noexcept {
        return get_num_companions() + get_num_prisoners();
    }

    [[nodiscard]] std::int32_t get_size_wo_prisoners() const noexcept {
        return get_num_companions();
    }

    [[nodiscard]] std::int32_t get_num_companion_stacks() const noexcept {
        std::int32_t n = 0;
        for (const auto& s : stacks) {
            if (!s.is_prisoner()) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] std::int32_t get_num_prisoner_stacks() const noexcept {
        std::int32_t n = 0;
        for (const auto& s : stacks) {
            if (s.is_prisoner()) {
                ++n;
            }
        }
        return n;
    }

    [[nodiscard]] std::int32_t get_troop_kind_count(std::int32_t troop_no) const noexcept {
        std::int32_t count = 0;
        for (const auto& s : stacks) {
            if (s.troop_no == troop_no && !s.is_prisoner()) {
                count += s.num_troops;
            }
        }
        return count;
    }

    std::int32_t add_members(std::int32_t troop_no,
                             std::int32_t count,
                             bool prisoner = false) {
        if (count <= 0) {
            return 0;
        }
        const std::uint32_t flag = prisoner ? 1U : 0U;
        std::int32_t before = 0;
        WsePartyStack* target = nullptr;
        for (auto& s : stacks) {
            if (s.troop_no == troop_no && (s.flags & 1U) == flag) {
                before += s.num_troops;
                if (target == nullptr) {
                    target = &s;
                }
            }
        }
        if (target != nullptr) {
            target->num_troops = saturating_add_i32(target->num_troops, count);
        } else {
            stacks.push_back(WsePartyStack{troop_no, count, 0, flag});
        }
        return before;
    }

    std::int32_t remove_members(std::int32_t troop_no,
                                std::int32_t count,
                                bool prisoner = false) {
        if (count <= 0) {
            return 0;
        }
        const std::uint32_t flag = prisoner ? 1U : 0U;
        for (std::size_t idx = stacks.size(); idx > 0; --idx) {
            auto& s = stacks[idx - 1];
            if (s.troop_no == troop_no && (s.flags & 1U) == flag) {
                const std::int32_t removed = std::min(count, s.num_troops);
                s.num_troops -= removed;
                if (s.num_wounded_troops > s.num_troops) {
                    s.num_wounded_troops = s.num_troops;
                }
                if (s.num_troops <= 0) {
                    stacks.erase(stacks.begin() + static_cast<std::ptrdiff_t>(idx - 1));
                }
                return removed;
            }
        }
        return 0;
    }

    // Mirrors mbParty::getSkill (0x1401bb220); falls back to skill_levels when stacks empty.
    [[nodiscard]] std::int32_t get_skill(std::int32_t skill_no,
                                         const std::vector<WseTroop>& troops,
                                         const std::array<WseSkill, kMaxSkills>& skills) const noexcept {
        if (skill_no < 0 || skill_no >= kMaxSkills) {
            return 0;
        }
        if (stacks.empty()) {
            return skill_levels[static_cast<std::size_t>(skill_no)];
        }
        const std::int32_t max_lvl =
            std::max<std::int32_t>(0, skills[static_cast<std::size_t>(skill_no)].max_level);
        const bool is_party =
            (skills[static_cast<std::size_t>(skill_no)].flags & kSkillPartyFlagBit) != 0U;
        auto troop_skill = [&](std::int32_t troop_no) -> std::int32_t {
            if (troop_no < 0 || static_cast<std::size_t>(troop_no) >= troops.size()) {
                return 0;
            }
            return std::min(troops[static_cast<std::size_t>(troop_no)].get_skill_raw(skill_no),
                            max_lvl);
        };
        std::int32_t leader_skill = 0;
        if (!stacks[0].is_prisoner() && stacks[0].unwounded() > 0) {
            leader_skill = troop_skill(stacks[0].troop_no);
        }
        std::int32_t best = leader_skill;
        if (is_party) {
            for (std::size_t i = 1; i < stacks.size(); ++i) {
                const auto& st = stacks[i];
                if (!st.is_prisoner() && st.unwounded() > 0 && st.troop_no >= 0 &&
                    static_cast<std::size_t>(st.troop_no) < troops.size() &&
                    troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
                    best = std::max(best, troop_skill(st.troop_no));
                }
            }
            std::int32_t bonus = 0;
            if (leader_skill >= 10) {
                bonus = 4;
            } else if (leader_skill >= 8) {
                bonus = 3;
            } else if (leader_skill >= 5) {
                bonus = 2;
            } else if (leader_skill >= 2) {
                bonus = 1;
            }
            best += bonus;
        }
        return best;
    }
};

struct WsePlayer {
    std::int32_t status = 2; // mbnetPlayer::m_status == 2 checked by 0x1401c56b0
    std::string name{};
    wse::vm::Slots slots{};
    std::int32_t team_no = 0;
    std::int32_t troop_no = -1;
    std::int32_t agent_no = -1;
    std::int32_t gold = 0;
    std::int32_t score = 0;
    std::int32_t kill_count = 0;
    std::int32_t death_count = 0;
    std::int32_t ping = 0;
    bool is_muted = false;
    bool is_admin = false;
    std::int32_t unique_id = 0;
    std::int32_t profile_gender = 0;
    std::int32_t banner_no = -1;
    std::array<std::int32_t, kNumTroopEquipmentSlots> item_ids{
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<bool, kNumTroopEquipmentSlots> picked_up_slots{};
    WseFaceKeys face_keys{};
    std::int32_t privilege = 0;
    WsePlayerM5Ext m5{};
};

struct WseTeam {
    wse::vm::Slots slots{};
    std::int32_t faction_no = 0;
    std::array<std::int32_t, kNumTeams> relations{};
    std::int32_t order_listener = -1;
    std::array<std::int32_t, kNumDivisions> order_types{};
    std::array<std::int32_t, kNumDivisions> order_subtypes{};
    WseTeamDivisionSubOrders sub_orders{};
    std::array<wse::core::WseTransform, kNumDivisions> order_positions{};
    std::int32_t leader_agent_no = -1;
    std::array<std::int32_t, kNumDivisions> division_leaders{-1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::int32_t bot_kill_count = 0;
    std::int32_t bot_death_count = 0;
    std::int32_t kill_count = 0;
    std::int32_t score = 0;
};

struct WseAgent {
    bool active = true;
    std::int32_t status = 1; // mbAgent::m_status: 1 = alive (0x14002b940), 3 = wounded (0x14002b950)
    wse::vm::Slots slots{};
    std::int32_t type = 0;  // mbAgent::m_type: 0 = human, 1 = horse (0x14002b960)
    std::int32_t horse_agent_no = -1;
    std::int32_t rider_agent_no = -1;
    std::int32_t party_no = -1;
    std::int32_t troop_no = -1;
    std::int32_t team_no = 0;
    std::int32_t division_no = 0;
    std::int32_t entry_no = 0;
    float hit_points = 100.0f;
    float max_hit_points = 100.0f;
    bool is_wounded = false;
    bool is_routed = false;
    bool is_defender = false;
    bool is_non_player = true;
    std::int32_t class_no = 0;
    std::int32_t kill_count = 0;
    std::int32_t wound_count = 0;
    std::array<std::int32_t, kNumTroopEquipmentSlots> equipped_items{
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    std::array<std::int32_t, kNumTroopEquipmentSlots> item_ammo{};
    std::array<std::int32_t, 2> wielded_items{-1, -1}; // [0]=hand 0 (weapon), [1]=hand 1 (shield)
    std::array<std::int32_t, kMaxAgentItems> attached_mission_object_nos{-1, -1, -1, -1};
    bool ai_alarmed = false;
    std::int32_t simple_behavior = 0;
    bool has_scripted_destination = false;
    float speed_limit = 0.0f;
    std::int32_t group_no = 0;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    wse::core::WseVec4 speed{};
    wse::core::WseTransform target_position = wse::core::WseTransform::identity();
    std::int32_t animation = -1;
    std::int32_t stand_animation = -1;
    std::int32_t player_no = -1;
    std::int32_t horse_item_no = -1;
    float action_speed_modifier = 1.0f;
    float damage_modifier = 1.0f;
    float accuracy_modifier = 1.0f;
    float speed_modifier = 1.0f;
    float reload_speed_modifier = 1.0f;
    float use_speed_modifier = 1.0f;
    float ranged_damage_modifier = 1.0f;
    bool visible = true;
    std::int32_t crouch_mode = 0;
    bool always_attack_in_melee = false;
    bool running_away = false;
    wse::core::WseVec4 run_away_target{};
    wse::core::WseTransform look_position = wse::core::WseTransform::identity();
    std::int32_t skin_no = 0;
    std::int32_t dna = -1;
    std::int32_t banner_tableau_material_no = -1;
    std::int32_t banner_tableau_param = 0;
    bool show_banner = false;
    bool has_relations = false;
    std::vector<std::pair<std::int32_t, std::int32_t>> relations{};
    bool camera_mode = false;
    WseFaceKeys face_keys{};

    // Mirrors mbAgent::isHuman (0x14002b960): m_type == 0.
    [[nodiscard]] constexpr bool is_human() const noexcept {
        return type == 0;
    }
    // Mirrors mbAgent::isAlive (0x14002b940): m_status == 1.
    [[nodiscard]] constexpr bool is_alive() const noexcept {
        return active && status == 1;
    }
    // Mirrors mbAgent::isWounded (0x14002b950): m_status == 3.
    [[nodiscard]] constexpr bool is_wounded_state() const noexcept {
        return status == 3 || is_wounded;
    }
    WseAgentM5Ext m5{};
};

struct WseMissionObject {
    bool active = true;
    wse::vm::Slots slots{};
    std::int32_t meta_mesh_no = -1;
    std::int32_t scene_prop_no = -1;
    std::int32_t variation_id_no = 0;
    std::int32_t variation_id_2_no = 0;
    bool is_item = false;
    float hit_points = 0.0f;
    float max_hit_points = 0.0f;
    std::int32_t parent_agent_no = -1;
    std::int32_t team_no = 0;
    bool visible = true;
    bool is_destructible = false;
    wse::core::WseTransform position = wse::core::WseTransform::identity();
    wse::core::WseTransform target_position = wse::core::WseTransform::identity();
    wse::core::WseVec4 scale{1.0f, 1.0f, 1.0f, 1.0f};
    wse::core::WseTransform starting_position = wse::core::WseTransform::identity();
    bool animating = false;
    float animate_duration = 0.0f;
    bool physics_enabled = true;
    float enable_timer = 0.0f;
    WseMissionObjectM5Ext m5{};
};

struct WseDisplayedMessage {
    std::u16string text{};
    std::uint32_t color = 0xffffffffu;
};

struct WseEditModeState {
    bool active = false;                                  // mbGame +0x16ed0
    std::int32_t mode = 0;                                // mbGame +0x16ed4
    bool dialog_open = false;                             // mbGame +0x16f30
    std::int32_t highlighted_index = -1;                  // mbGame +0x16f34
    std::vector<std::int32_t> selected_prop_instances{};  // mbGame +0x16f38
    bool selection_changed = false;                       // mbGame +0x16f50
};

namespace container_detail {

// Mirrors rglStableVector<T>::createItem (`0x14014bae0` mbAgent / `0x14014c1b0` mbMissionObject):
// reuses freed interior slots LIFO while keeping active indices < max_entries stable.
template <typename T>
[[nodiscard]] inline std::int32_t allocate_stable_entry(
    std::vector<T>& vec,
    std::vector<std::int32_t>& free_slots,
    std::size_t max_entries,
    T&& entry) {
    while (!free_slots.empty()) {
        const std::int32_t recycled = free_slots.back();
        free_slots.pop_back();
        if (recycled >= 0 && static_cast<std::size_t>(recycled) < vec.size() &&
            !vec[static_cast<std::size_t>(recycled)].active) {
            vec[static_cast<std::size_t>(recycled)] = std::move(entry);
            vec[static_cast<std::size_t>(recycled)].active = true;
            return recycled;
        }
    }
    if (vec.size() >= max_entries) {
        return -1;
    }
    const auto new_id = static_cast<std::int32_t>(vec.size());
    vec.push_back(std::move(entry));
    vec.back().active = true;
    return new_id;
}

// Mirrors rglStableVector<T>::remove (`0x14014b990` mbAgent / `0x14014bf40` mbMissionObject):
// resets the deactivated slot, shrinks trailing inactive capacity to the new high-water
// mark, and records interior holes on the LIFO free list. The free list is not scanned for
// duplicates (that made mass removal O(n^2)): a slot is pushed only on an active->inactive
// transition, and `allocate_stable_entry` skips any stale entry whose slot is active again.
template <typename T>
inline bool release_stable_entry(std::vector<T>& vec,
                                 std::vector<std::int32_t>& free_slots,
                                 std::int32_t id) {
    if (id < 0 || static_cast<std::size_t>(id) >= vec.size()) {
        return false;
    }
    auto& elem = vec[static_cast<std::size_t>(id)];
    if (!elem.active) {
        return false;
    }
    elem = T{};
    elem.active = false;
    if (static_cast<std::size_t>(id) + 1U == vec.size()) {
        while (!vec.empty() && !vec.back().active) {
            vec.pop_back();
        }
        const auto new_limit = static_cast<std::int32_t>(vec.size());
        std::erase_if(free_slots, [new_limit](std::int32_t slot) noexcept {
            return slot < 0 || slot >= new_limit;
        });
    } else {
        free_slots.push_back(id);
    }
    return true;
}

template <typename T>
inline void rebuild_stable_free_list(std::vector<T>& vec,
                                     std::vector<std::int32_t>& free_slots) {
    free_slots.clear();
    while (!vec.empty() && !vec.back().active) {
        vec.pop_back();
    }
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (!vec[i].active) {
            free_slots.push_back(static_cast<std::int32_t>(i));
        }
    }
}

}  // namespace container_detail

struct WseWorldState {
    bool has_game = true;
    std::int32_t main_party_no = 0;                       // mbGame +0x0420
    std::int32_t player_troop_no = 0;                     // mbGame::m_playerTroopNo
    std::int32_t player_agent_no = -1;                    // mbMission::m_playerAgentNo
    std::int32_t player_agent_own_troop_kill_count = 0;
    std::int32_t player_agent_own_troop_wound_count = 0;
    std::int32_t main_party_side = 0;                     // mbMission::m_mainPartySide
    std::int32_t enemy_side = 1;                          // mbMission::m_enemySide
    std::int64_t campaign_time = 0;                       // DAT_140620038
    std::int32_t current_hours = 0;                       // mbGame +hours (0x1401de3ed)
    std::int32_t game_year = 1257;                        // mbGame::m_year (0x1401cdf69)
    bool paused = false;                                  // mbGame +0x043c
    bool hide_hud = false;                                // DAT_140658140
    bool show_messages = true;                            // g_windowManager.m_displayMessages
    float water_level = 0.0f;                             // mbMission +0xa4f4
    float ground_level_z = 0.0f;                          // default terrain Z for ground probe
    std::int32_t max_players = kMaxPlayers;               // rglConfig::Network::iNumPlayersLimit
    std::int32_t num_skins = 2;                           // g_faceGenerator.m_numSkins
    std::int32_t attribute_limit = 64;                    // rglConfig::General::iAttributeLimit
    std::int32_t site_no_being_modified = -1;             // g_siteNoBeingModified
    std::int32_t conversation_partner_troop_no = -1;      // g_conversationManager.m_partnerTroopNo
    std::int32_t partner_faction_no = -1;                 // g_conversationManager.m_partnerFactionNo
    std::int32_t active_presentation_no = -1;
    std::int32_t presentation_duration = 0;
    bool presentation_background = false;
    bool order_menu_open = false;
    std::int32_t order_menu_type = -2;
    std::int32_t order_menu_subtype = -1;
    std::int32_t mission_result = 0;
    bool mission_finished = false;
    float mission_finish_delay = 0.0f;
    std::int32_t jump_mission_no = -1;
    std::int32_t jump_menu_no = -1;
    std::int64_t trigger_result = 0;
    std::array<bool, kNumUseTooltipTypes> use_objects_tooltip_enabled{};
    std::array<WseEntryPoint, kMaxEntryPoints> entry_points{};
    std::array<WseMissionTimer, 3> mission_timers{};
    WseEditModeState edit_mode{};

    std::array<WseSkill, kMaxSkills> skills{};            // 0x140660840
    std::vector<WseItemKind> items{};
    std::vector<WsePartyTemplate> party_templates{};
    std::vector<WseTroop> troops{};
    std::vector<WseFaction> factions{};
    std::vector<WseQuest> quests{};
    std::vector<WseSite> sites{};
    std::vector<WseParty> parties{};
    std::int32_t next_party_key = 0;  // rglHashVector<mbParty>::m_nextKey (+0x138f0)
    std::vector<WsePlayer> players{};
    std::array<WseTeam, kNumTeams> teams{};
    std::vector<WseAgent> agents{};
    std::vector<WseMissionObject> mission_objects{};
    std::vector<std::int32_t> agent_free_slots{};           // rglStableVector<mbAgent> LIFO pool
    std::vector<std::int32_t> mission_object_free_slots{};  // rglStableVector<mbMissionObject> LIFO pool
    std::vector<WseMissionTemplate> mission_templates{};
    std::vector<WsePresentationOverlay> overlays{};
    std::vector<WseMultiplayerMessage> multiplayer_messages{};
    std::vector<WseBattleEvent> played_sounds{};
    std::int32_t num_particle_systems = 0;                // DAT_140624a40

    WseMetaMesh* cur_item_meta_mesh = nullptr;            // 0x1401c5fc0 (caller -50)
    std::unordered_map<std::string, std::vector<WseMesh>> resource_meshes{};
    std::unordered_map<std::string, bool> resource_materials{};
    std::unordered_map<std::string, bool> resource_skeleton_models{};

    // Case-sensitive INI/variable/localization stores (0x1402a8010, 0x1402aa670, 0x14065eeb0).
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>
        module_settings{};
    std::unordered_map<std::string, std::string> game_variables{};
    std::unordered_map<std::string, std::string> translations{};
    // Savegame directory read by str_store_savegame_md5; null means "no
    // savegames". Not owned: the embedder keeps the Vfs alive.
    wse::platform::Vfs* savegames_vfs = nullptr;
    // Non-owning audio runtime hook driving sound/music opcodes when attached.
    WseAudioRuntime* audio_runtime = nullptr;
    std::vector<WseDisplayedMessage> displayed_messages{};
    bool is_multiplayer = false;
    bool is_server = false;
    bool is_dedicated_server = false;
    std::int32_t my_player_no = -1;
    bool map_free_state = true;
    bool in_meta_mission = false;
    bool conversation_screen_active = false;
    bool is_trial_version = false;
    bool edit_mode_enabled = false;
    std::int32_t reduce_campaign_ai = 2;
    std::string result_string{};
    std::int32_t encountered_party_no = -1;
    std::int32_t encountered_party_2_no = -1;
    bool encountered_party_is_attacker = false;
    std::int32_t camera_follow_party_no = -1;
    bool party_battle_mode = false;
    std::int32_t party_creation_min_random = 0;
    std::int32_t party_creation_max_random = 100;
    float spawn_radius = 3.0f;
    std::int32_t battle_advantage = 0;
    std::int32_t current_day = 0;
    bool currently_night = false;
    float scene_day_time = 12.0f;
    bool talk_enabled = true;
    float mission_time_speed = 1.0f;
    float mission_time_speed_target = 1.0f;
    float mission_time_speed_duration = 0.0f;
    bool mission_duel_mode = false;
    bool cheer_at_no_enemy = true;
    float zoom_amount = 0.0f;
    bool zoom_disabled = false;
    std::int32_t jump_scene_no = -1;
    std::int32_t jump_entry_no = 0;
    std::int32_t current_site_no = -1;
    std::int32_t current_mission_template_no = -1;
    wse::core::WseTransform spawn_position = wse::core::WseTransform::identity();
    wse::core::WseVec2 scene_bound_min{-100.0f, -100.0f};
    wse::core::WseVec2 scene_bound_max{100.0f, 100.0f};
    std::array<std::string, kNumDivisions> class_names{
        "Infantry", "Archers", "Cavalry", "Unnamed 1", "Unnamed 2",
        "Unnamed 3", "Unnamed 4", "Unnamed 5", "Unnamed 6"};
    std::unordered_map<std::int32_t, std::int32_t> achievement_stats{};
    std::unordered_map<std::int32_t, bool> unlocked_achievements{};
    WseInputState input{};
    WseMusicState music{};
    WseTableauState tableau{};
    WseLightingState lighting{};
    WseDialogState dialog{};
    WseUiPanelState ui{};
    WseSceneFxState fx{};
    WseMpSessionState mp{};
    WseMiscRuntimeState misc{};
    WseM5WorldExt m5{};
    WseM6WorldExt m6{};
    std::vector<WseInfoPage> info_pages{};
    std::vector<WseBattleEvent> battle_events{};
    std::vector<std::string> mesh_names{};
    std::array<WseItemModifierStats, static_cast<std::size_t>(kNumItemModifiers)> item_modifiers{};

    // Whole-savegame framing + Phase 2C queued-window runtime fields (v1.1.5.6 PE/PDB).
    std::int32_t savegame_no = 0;                         // mbGame +0x0000
    std::int32_t seed = 0;                                // mbGame +0x0004
    std::string savegame_name{};                          // mbGame +0x0008
    std::int32_t module_version = 0;                      // g_moduleVersion (0x1406200c8)
    std::int32_t compatible_savegame_module_version = 0;  // g_compatibleSavegameModuleVersion (0x1406200c0)
    std::uint32_t encounter_flags = 0;                    // mbGame +0x0454
    std::int32_t default_prisoner_price = 0;              // mbGame +0x13e58
    std::uint32_t tutorial_flags = 0;                     // mbGame +0x13f98
    std::int32_t rest_no_attack = 0;                      // mbGame +0x13f84
    float rest_period = 0.0f;                             // mbGame +0x13f80
    std::int32_t screen_troop_no = -1;                    // mbGame +0x13fa0
    std::int32_t mission_template_no = -1;                // mbGame +0x13fac
    std::int32_t date_timer_no = 0;                       // mbGame +0x13fc0
    std::int64_t date_timer_value = 0;                    // mbGame +0x13fc8
    std::int32_t game_day = 0;                            // mbGame +0x13fd8
    std::int32_t game_week = 0;                           // mbGame +0x13fdc
    std::int32_t game_month = 0;                          // mbGame +0x13fe0
    float cloud_amount = 0.0f;                            // mbGame +0x13fe8
    float haze_amount = 0.0f;                             // mbGame +0x13fec
    std::string game_log{};                               // mbGame +0x14000
    std::int32_t combat_damage_to_self = 0;               // 0..2 from fDamageToSelf
    std::int32_t combat_damage_to_friends = 0;            // 0..2 from fDamageToFriends
    std::int32_t reduce_battle_ai = 0;                    // iReduceBattleAi
    std::int32_t combat_speed = 2;                        // iCombatSpeed
    std::int32_t player_kill_count = 0;                   // mbGame +0x0440
    std::int32_t player_teamkill_count = 0;               // mbGame +0x0444
    std::int32_t player_wound_count = 0;                  // mbGame +0x0448
    std::int32_t player_teamwound_count = 0;              // mbGame +0x044c
    std::vector<WseSavegameTrack> tracks{};               // mbGame +0x13eb8
    std::vector<WseSavegameEvent> events{};               // mbGame +0x04c8
    std::array<std::int32_t, 32 * 32> main_party_dnas{};  // mbGame +0x15ec8

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
    [[nodiscard]] bool is_valid_overlay(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < overlays.size();
    }
    [[nodiscard]] bool is_valid_entry_point(std::int32_t id) const noexcept {
        return id >= 0 && id < kMaxEntryPoints &&
               entry_points[static_cast<std::size_t>(id)].valid;
    }
    [[nodiscard]] bool is_valid_mission_template(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < mission_templates.size();
    }
    [[nodiscard]] bool is_valid_particle_system(std::int32_t id) const noexcept {
        return id >= 0 && id < num_particle_systems;
    }
    [[nodiscard]] bool is_valid_info_page(std::int32_t id) const noexcept {
        return has_game && id >= 0 && static_cast<std::size_t>(id) < info_pages.size();
    }
    [[nodiscard]] bool is_valid_music_track(std::int32_t id) const noexcept {
        return id >= 0 && id < misc.num_music_tracks;
    }
    [[nodiscard]] bool is_valid_map_icon(std::int32_t id) const noexcept {
        return id >= 0 && id < misc.num_map_icons;
    }
    [[nodiscard]] bool is_valid_tableau_material(std::int32_t id) const noexcept {
        return id >= 0 && id < misc.num_tableau_materials;
    }
    [[nodiscard]] bool is_valid_mesh(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < mesh_names.size();
    }
    [[nodiscard]] bool is_valid_scene_prop(std::int32_t id) const noexcept {
        return id >= 0 && id < misc.num_scene_props;
    }
    [[nodiscard]] bool is_valid_action(std::int32_t id) const noexcept {
        return id >= 0 && id < misc.num_actions;
    }
    [[nodiscard]] bool is_valid_menu(std::int32_t id) const noexcept {
        return id >= 0 && id < misc.num_menus;
    }
    [[nodiscard]] bool is_valid_post_effect(std::int32_t id) const noexcept {
        return id >= 0 && id < misc.num_post_effects;
    }
    [[nodiscard]] bool is_valid_profile(std::int32_t id) const noexcept {
        return id >= 0 && static_cast<std::size_t>(id) < mp.profiles.size();
    }

    // Queues a window transition (`mbGame::enqueueWindow`, callee of `change_screen_*`,
    // `jump_to_menu` `0x1401dc431`, and `start_presentation` `0x1401cc082`).
    // Safety deviation: retail has no cap; the oldest entry is dropped once
    // `kMaxQueuedWindows` are pending so a script that queues windows every frame while
    // nothing consumes them cannot grow the queue without bound.
    void enqueue_window(std::int32_t win_type, std::int32_t param = -1) {
        if (m5.enqueued_windows.size() >= kMaxQueuedWindows) {
            m5.enqueued_windows.erase(m5.enqueued_windows.begin());
        }
        m5.enqueued_windows.emplace_back(win_type, param);
    }

    // Consumes the back queued campaign window transition (`mbMapWindow::frameMove` `0x1401180f0`,
    // `0x140118ae7..0x140118be0`): 0x12 opens `mbMenuWindow` 0x12 and clears remaining entries per
    // `mbMenuWindow::open` `0x1401203a0`, 0x0f pops back and opens presentation window `0x16`, and
    // 0x10 pops back and opens notes window `0x15`. Syncs `m5.active_window` and
    // `m6.ui.active_window_id`.
    // Safety deviation: in retail every other type stays queued until the window it targets (menu,
    // inventory, party, ... screens) consumes it, but the engine models none of those windows, so
    // nothing else would ever pop them. The back entry is popped here instead and the active
    // window becomes unknown (-1, which every window-gated WSE opcode accepts), so for example
    // `change_screen_map` / `change_screen_return` from an open menu stop the menu window from
    // gating map-only opcodes (`get_mouse_map_coordinates` 3039) for the rest of the session.
    std::int32_t consume_campaign_queued_window() noexcept {
        if (m5.enqueued_windows.empty()) {
            return -1;
        }
        const auto [win_type, param] = m5.enqueued_windows.back();
        std::int32_t screen_win = -1;
        switch (win_type) {
        case kQueuedWindowMenuType:
            m5.enqueued_windows.clear();
            screen_win = kScreenWindowMenu;
            ++m6.ui.menu_open_serial;
            break;
        case kQueuedWindowPresentationType:
            m5.enqueued_windows.pop_back();
            if (param >= 0) {
                active_presentation_no = param;
            }
            screen_win = kScreenWindowPresentation;
            break;
        case kQueuedWindowNotesType:
            m5.enqueued_windows.pop_back();
            screen_win = kScreenWindowNotes;
            break;
        default:
            m5.enqueued_windows.pop_back();
            break;
        }
        m5.active_window = screen_win;
        m6.ui.active_window_id = screen_win;
        return screen_win;
    }

    // Allocates a fresh party with monotonic script ID (mirrors rglHashVector<mbParty>::getFreeItem
    // `0x1400e8890` monotonic key `m_nextKey` `+0x138f0`). Removed parties stay as tombstones, and
    // loaded saves whose `m_nextKey` exceeds `m_numSlots` resume allocating from `next_party_key`.
    // Returns -1 once `kMaxPartyKeys` keys have been handed out (safety deviation, see there).
    [[nodiscard]] std::int32_t allocate_party(WseParty party) {
        const auto new_id =
            std::max(next_party_key, static_cast<std::int32_t>(parties.size()));
        if (new_id < 0 || new_id >= kMaxPartyKeys) {
            return -1;
        }
        party.active = true;
        if (static_cast<std::size_t>(new_id) > parties.size()) {
            const std::size_t old_size = parties.size();
            parties.resize(static_cast<std::size_t>(new_id));
            for (std::size_t i = old_size; i < parties.size(); ++i) {
                parties[i].active = false;
            }
        }
        parties.push_back(std::move(party));
        next_party_key = new_id + 1;
        return new_id;
    }

    // Detaches `party_no` from any current parent party cleanly (keeping bidirectional
    // parent `attached_party_nos` and child `attached_to_party_no` in sync).
    bool detach_party(std::int32_t party_no) {
        if (!is_valid_party(party_no)) {
            return false;
        }
        auto& p = parties[static_cast<std::size_t>(party_no)];
        if (p.attached_to_party_no >= 0 &&
            static_cast<std::size_t>(p.attached_to_party_no) < parties.size()) {
            auto& parent = parties[static_cast<std::size_t>(p.attached_to_party_no)];
            std::erase(parent.attached_party_nos, party_no);
        }
        p.attached_to_party_no = -1;
        return true;
    }

    // True when `ancestor_no` is `party_no` or lies on its `attached_to_party_no` chain.
    // The walk is bounded by `parties.size()` so a pre-existing cycle cannot hang it.
    [[nodiscard]] bool party_is_attached_under(std::int32_t party_no,
                                               std::int32_t ancestor_no) const noexcept {
        std::int32_t cur = party_no;
        for (std::size_t steps = 0; steps <= parties.size(); ++steps) {
            if (cur == ancestor_no) {
                return true;
            }
            if (cur < 0 || static_cast<std::size_t>(cur) >= parties.size()) {
                return false;
            }
            cur = parties[static_cast<std::size_t>(cur)].attached_to_party_no;
        }
        return false;
    }

    // Attaches `party_no` to `target_no`, detaching any prior parent first.
    // Safety deviation: refuses an attachment that would close a cycle (`target_no` already
    // attached under `party_no`), so attachment walks always terminate.
    bool attach_party(std::int32_t party_no, std::int32_t target_no) {
        if (!is_valid_party(party_no) || !is_valid_party(target_no) ||
            party_is_attached_under(target_no, party_no)) {
            return false;
        }
        (void)detach_party(party_no);
        auto& p = parties[static_cast<std::size_t>(party_no)];
        auto& target = parties[static_cast<std::size_t>(target_no)];
        p.attached_to_party_no = target_no;
        if (std::find(target.attached_party_nos.begin(), target.attached_party_nos.end(),
                      party_no) == target.attached_party_nos.end()) {
            target.attached_party_nos.push_back(party_no);
        }
        return true;
    }

    // Party teardown matching `mbGame::removeParty` (`0x1400ddaf0`, also tail-called by
    // `mbGame::addPartyToParty` `0x1400de390` at `0x1400de5e0` for `party_join*`): detaches
    // parent/child links, increments template `num_destroyed`, clears AI behavior targets, unlinks
    // agent/player/world references, marks `pf_disabled` (`0x100`), frees buffers, and keeps
    // removed IDs as tombstones so script handles are never reused.
    // Safety deviation: retail `0x1400ddaf0` does not reject `p_main_party` (the Warband SDK warns
    // removing non-spawned/main parties corrupts the game), whereas `remove_party` returns false.
    bool remove_party(std::int32_t party_no) {
        if (party_no == main_party_no || !detach_party(party_no)) {
            return false;
        }
        auto& p = parties[static_cast<std::size_t>(party_no)];
        for (const std::int32_t child_no : p.attached_party_nos) {
            if (child_no >= 0 && static_cast<std::size_t>(child_no) < parties.size()) {
                auto& child = parties[static_cast<std::size_t>(child_no)];
                if (child.attached_to_party_no == party_no) {
                    child.attached_to_party_no = -1;
                }
            }
        }
        p.attached_party_nos.clear();
        if (is_valid_party_template(p.template_no)) {
            ++party_templates[static_cast<std::size_t>(p.template_no)].num_destroyed;
        }
        for (std::size_t i = 0; i < parties.size(); ++i) {
            if (i == static_cast<std::size_t>(party_no) || !parties[i].active) {
                continue;
            }
            if (parties[i].behavior_object == party_no) {
                parties[i].behavior_object = -1;
            }
            if (parties[i].default_behavior_object == party_no) {
                parties[i].default_behavior_object = -1;
            }
        }
        for (auto& ag : agents) {
            if (ag.active && ag.party_no == party_no) {
                ag.party_no = -1;
            }
        }
        for (auto& [pid, pl_ext] : m6.players) {
            (void)pid;
            if (pl_ext.party_no == party_no) {
                pl_ext.party_no = -1;
            }
        }
        p.player_no = -1;
        if (encountered_party_no == party_no) {
            encountered_party_no = -1;
        }
        if (encountered_party_2_no == party_no) {
            encountered_party_2_no = -1;
        }
        if (camera_follow_party_no == party_no) {
            camera_follow_party_no = -1;
        }
        p.flags |= 0x100ULL;
        p.stacks.clear();
        p.slots.clear();
        p.active = false;
        return true;
    }

    // Allocates an agent through `rglStableVector<mbAgent>` LIFO recycling (`0x14014bae0`).
    [[nodiscard]] std::int32_t allocate_agent(WseAgent agent) {
        const std::int32_t id = container_detail::allocate_stable_entry(
            agents, agent_free_slots, kMaxSpawnedAgents, std::move(agent));
        if (id >= 0) {
            m6.agents.erase(id);
        }
        return id;
    }

    // Clears every world-held reference to agent `agent_no` (all of them when `agent_no` is -1).
    // Agent slots are recycled, so a stale id would otherwise alias the next agent spawned in
    // that slot (team leader, look/AI target, horse, camera target, conversation speaker, ...).
    // `fx.missiles` (append-only launch log) and `fx.ray_obstacles` (static ray-cast geometry)
    // hold no recycled agent slots and stay untouched; `fx.trigger_agent_no` clears on full wipe.
    void unlink_agent_references(std::int32_t agent_no) {
        const auto matches = [agent_no](std::int32_t ref) noexcept {
            return ref >= 0 && (agent_no < 0 || ref == agent_no);
        };
        const auto reset = [&matches](std::int32_t& ref) noexcept {
            if (matches(ref)) {
                ref = -1;
            }
        };
        if (agent_no < 0) {
            fx.trigger_agent_no = -1;
        }
        for (auto& other : agents) {
            reset(other.horse_agent_no);
            reset(other.rider_agent_no);
            reset(other.m5.look_target_agent_no);
            reset(other.m5.ai_move_target_agent_no);
            reset(other.m5.ai_behavior_target_agent_no);
            std::erase_if(other.m5.offers, [&matches](const WseAgentOffer& offer) noexcept {
                return matches(offer.agent_no);
            });
            std::erase_if(other.m5.cached_enemies, matches);
        }
        for (auto& obj : mission_objects) {
            reset(obj.parent_agent_no);
        }
        for (auto& team : teams) {
            reset(team.leader_agent_no);
            reset(team.order_listener);
            for (auto& leader : team.division_leaders) {
                reset(leader);
            }
        }
        for (auto& pl : players) {
            reset(pl.agent_no);
        }
        reset(player_agent_no);
        reset(m6.scene.spectated_agent_no);
        reset(m5.mission_camera.target_agent_no);
        reset(dialog.speaker_agent_no);
        reset(dialog.pending_mission_agent_no);
    }

    // Full container removal matching `mbMission::removeAgent` (`0x140138710`) +
    // `rglStableVector<mbAgent>::remove` (`0x14014b990`): unlinks every world reference
    // (`unlink_agent_references`), clears M6 ext state, and recycles/trims the backing slot.
    bool remove_agent(std::int32_t agent_no) {
        if (!is_valid_agent(agent_no)) {
            return false;
        }
        unlink_agent_references(agent_no);
        m6.agents.erase(agent_no);
        return container_detail::release_stable_entry(agents, agent_free_slots, agent_no);
    }

    // Allocates a mission object via `rglStableVector<mbMissionObject>` recycling (`0x14014c1b0`).
    [[nodiscard]] std::int32_t allocate_mission_object(WseMissionObject obj) {
        const std::int32_t id = container_detail::allocate_stable_entry(
            mission_objects, mission_object_free_slots, kMaxSpawnedMissionObjects,
            std::move(obj));
        if (id >= 0) {
            m6.mission_objects.erase(id);
        }
        return id;
    }

    // Removes a mission object matching `mbMission::removeMissionObject` (`0x140132d20`) +
    // `rglStableVector<mbMissionObject>::remove` (`0x14014bf40`), first clearing every
    // world reference so a recycled slot does not inherit them.
    bool remove_mission_object(std::int32_t obj_no) {
        if (!is_valid_mission_object(obj_no)) {
            return false;
        }
        for (auto& slot_obj : fx.spawn_effector_mission_object_nos) {
            if (slot_obj == obj_no) {
                slot_obj = -1;
            }
        }
        for (auto& ag : agents) {
            for (auto& att : ag.attached_mission_object_nos) {
                if (att == obj_no) {
                    att = -1;
                }
            }
            if (ag.m5.standing_mission_object_no == obj_no) {
                ag.m5.standing_mission_object_no = -1;
            }
        }
        std::erase_if(m5.attached_missiles, [obj_no](const WseAttachedMissile& m) noexcept {
            return m.mission_object_no == obj_no;
        });
        std::erase(edit_mode.selected_prop_instances, obj_no);
        m6.mission_objects.erase(obj_no);
        return container_detail::release_stable_entry(
            mission_objects, mission_object_free_slots, obj_no);
    }

    // Removes every agent at once (no per-agent sweep) and clears all agent references.
    void clear_agents() {
        unlink_agent_references(-1);
        agents.clear();
        agent_free_slots.clear();
        m6.agents.clear();
    }

    // Drops every agent and mission object (module reset / new scene).
    void clear_mission_entities() {
        clear_agents();
        mission_objects.clear();
        mission_object_free_slots.clear();
        m6.mission_objects.clear();
        m5.attached_missiles.clear();
        edit_mode.selected_prop_instances.clear();
        fx.spawn_effector_mission_object_nos.fill(-1);
    }

    // `multiplayer_clear_scene` (416, `mbMission::clearScene` `0x140147c00`): removes every agent,
    // removes mission objects marked prunable (`+0x1d5` / `m5.enable_pruning`, loop at
    // `0x14014801f`) while leaving scene-placed and unpruned props (`spawn_scene_prop` `0x14013a5d0`,
    // `spawn_item(..., 0)`) untouched in state, and resets active players' `agent_no = -1` /
    // `ready_to_spawn = false` (`+0x1f4`/`+0x1f8` at `0x140148218`) without wiping `item_ids`.
    void clear_multiplayer_scene() {
        clear_agents();
        for (auto& pl : players) {
            if (pl.status == 2) {
                pl.agent_no = -1;
                pl.m5.ready_to_spawn = false;
            }
        }
        for (std::int32_t i = static_cast<std::int32_t>(mission_objects.size()) - 1; i >= 0; --i) {
            const auto idx = static_cast<std::size_t>(i);
            if (idx < mission_objects.size() && mission_objects[idx].active &&
                mission_objects[idx].m5.enable_pruning) {
                (void)remove_mission_object(i);
            }
        }
    }

    // Advances entity lifetimes: removes agents whose fade-out (`agent_fade_out`, 1749) is
    // older than `30.0s` (`mbAgent::frameMove` `0x14002a270`, check at `0x14002a3f1`) and
    // spawned items whose prune timer ran out. A faded-out scene prop is NOT removed:
    // `scene_prop_fade_in` (1823) must be able to bring it back, and its id must stay valid.
    void advance_mission_lifetimes(const std::array<std::int64_t, 3>& timers_total,
                                   float dt_seconds) {
        static constexpr std::int64_t kAgentFadeOutTicks = 3000000LL;  // 30.0s * 100k ticks/s
        for (std::int32_t i = static_cast<std::int32_t>(agents.size()) - 1; i >= 0; --i) {
            const auto idx = static_cast<std::size_t>(i);
            if (idx >= agents.size() || !agents[idx].active) {
                continue;
            }
            const auto& m5_ext = agents[idx].m5;
            if (m5_ext.exit_status == 3 &&
                timers_total[2] >= m5_ext.exit_timer_start &&
                (timers_total[2] - m5_ext.exit_timer_start) >= kAgentFadeOutTicks) {
                (void)remove_agent(i);
            }
        }
        if (dt_seconds <= 0.0f) {
            return;
        }
        for (std::int32_t i = static_cast<std::int32_t>(mission_objects.size()) - 1; i >= 0; --i) {
            const auto idx = static_cast<std::size_t>(i);
            if (idx >= mission_objects.size() || !mission_objects[idx].active) {
                continue;
            }
            auto& m5_ext = mission_objects[idx].m5;
            if (m5_ext.enable_pruning && m5_ext.prune_time > 0.0f) {
                m5_ext.prune_time -= dt_seconds;
                if (m5_ext.prune_time <= 0.0f) {
                    (void)remove_mission_object(i);
                }
            }
        }
    }

    // Per-frame world housekeeping run after the trigger managers: the campaign map consumes
    // one queued window; a mission advances agent fade-out and item pruning.
    // A mission frame also retires campaign-map windows (map, menu, notes, presentation): the
    // tactical window replaced them, and the engine cannot tell whether the tactical (10) or
    // conversation (12) window is up, so the active window becomes unknown (-1) rather than
    // keeping a stale campaign window that would block mission-only opcodes (4901, 4907, 3044).
    void step_world_frame(const std::array<std::int64_t, 3>& timers_total, float dt_seconds) {
        if (lighting.has_scene) {
            switch (m6.ui.active_window_id) {
            case kScreenWindowMap:
            case kScreenWindowMenu:
            case kScreenWindowNotes:
            case kScreenWindowPresentation:
                m6.ui.active_window_id = -1;
                m5.active_window = -1;
                break;
            default:
                break;
            }
            advance_mission_lifetimes(timers_total, dt_seconds);
        } else {
            (void)consume_campaign_queued_window();
        }
    }

    void rebuild_stable_free_lists() {
        container_detail::rebuild_stable_free_list(agents, agent_free_slots);
        container_detail::rebuild_stable_free_list(mission_objects, mission_object_free_slots);
    }

    // Resets one player slot matching `mbnetPlayer::initialize` (`0x1401b73a0`) /
    // `clear` (`0x1401b7700`): unlinks agent & party links and clears scores, items, and slots.
    void clear_player_slot(std::int32_t player_no, bool preserve_temp_ban = false) {
        if (player_no < 0 || static_cast<std::size_t>(player_no) >= players.size()) {
            return;
        }
        auto& pl = players[static_cast<std::size_t>(player_no)];
        if (pl.agent_no >= 0 && static_cast<std::size_t>(pl.agent_no) < agents.size()) {
            auto& ag = agents[static_cast<std::size_t>(pl.agent_no)];
            if (ag.player_no == player_no) {
                ag.player_no = -1;
                ag.is_non_player = true;
            }
        }
        std::int32_t temp_ban_mins = 0;
        if (const auto it = m6.players.find(player_no); it != m6.players.end()) {
            if (preserve_temp_ban) {
                temp_ban_mins = it->second.temp_banned_minutes;
            }
            const std::int32_t old_party = it->second.party_no;
            if (old_party >= 0 && static_cast<std::size_t>(old_party) < parties.size() &&
                parties[static_cast<std::size_t>(old_party)].player_no == player_no) {
                parties[static_cast<std::size_t>(old_party)].player_no = -1;
            }
            m6.players.erase(it);
        }
        if (preserve_temp_ban && temp_ban_mins > 0) {
            m6.players[player_no].temp_banned_minutes = temp_ban_mins;
        }
        pl = WsePlayer{};
        pl.status = 0;
        pl.name = "UNASSIGNED";
        pl.team_no = 2;
    }

    // Initializes fixed-capacity `mbnetData` roster (`mbnetPlayer::initialize` `0x1401b73a0`).
    void init_players(std::int32_t capacity = kMaxPlayers) {
        const std::int32_t capped =
            std::clamp(capacity, std::int32_t{0}, kMaxPlayers);
        max_players = capped;
        players.assign(static_cast<std::size_t>(capped), WsePlayer{});
        for (std::int32_t i = 0; i < capped; ++i) {
            clear_player_slot(i);
        }
    }

    // Activates player slot `player_no` (`mbnetPlayer::create` `0x1401b8b00`).
    bool connect_player(std::int32_t player_no,
                        std::string name,
                        std::int32_t unique_id = 0,
                        std::int32_t team_no = 2) {
        if (player_no < 0 || player_no >= max_players ||
            static_cast<std::size_t>(player_no) >= players.size()) {
            return false;
        }
        clear_player_slot(player_no);
        auto& pl = players[static_cast<std::size_t>(player_no)];
        pl.status = 2;
        pl.name = name.empty() ? std::string("UNASSIGNED") : std::move(name);
        pl.unique_id = unique_id;
        pl.team_no = (team_no >= 0 && team_no < kNumTeams) ? team_no : 2;
        return true;
    }

    // Disconnects player `player_no` (`mbnetNetworkServer::disconnectPlayer` `0x1401a8da0` ->
    // `mbnetData::disconnectPlayer` `0x14017c7d0`). `mp.saved_ban` is deliberately kept:
    // `save_ban_info_of_player` (467) exists so `ban_player_using_saved_ban_info` (468) still
    // works after the player has left or been kicked while a ban poll runs.
    bool disconnect_player(std::int32_t player_no, bool preserve_temp_ban = false) {
        if (player_no < 0 || static_cast<std::size_t>(player_no) >= players.size()) {
            return false;
        }
        clear_player_slot(player_no, preserve_temp_ban);
        return true;
    }

    // Resets per-mission player equipment/agent links (`startMission` `0x14017c940`).
    void reset_players_for_mission() noexcept {
        for (std::size_t i = 0; i < players.size(); ++i) {
            auto& pl = players[i];
            if (pl.agent_no >= 0 && static_cast<std::size_t>(pl.agent_no) < agents.size()) {
                auto& ag = agents[static_cast<std::size_t>(pl.agent_no)];
                if (ag.player_no == static_cast<std::int32_t>(i)) {
                    ag.player_no = -1;
                    ag.is_non_player = true;
                }
            }
            pl.agent_no = -1;
            pl.item_ids.fill(-1);
            pl.picked_up_slots.fill(false);
            pl.m5.spawn_items_saved = false;
            pl.m5.saved_spawn_items_value = 0;
            pl.m5.ready_to_spawn = false;
        }
    }
};

}  // namespace wse::game

#include "wse/game/world_campaign_helpers.h"
