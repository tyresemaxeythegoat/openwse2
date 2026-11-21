// Campaign and inventory helpers for WseWorldState, WseParty, and WseTroop.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "wse/game/world_state.h"

namespace wse::game {

namespace detail {

// Default MSVC-compatible LCG fallback matching rglRand (0x140330d70) for direct C++ callers
// that do not supply a VmContext RNG callback.
inline std::int32_t default_campaign_rand(std::uint32_t& state, std::int32_t max_val) noexcept {
    if (max_val <= 1) {
        return 0;
    }
    state = state * 0x343fdU + 0x269ec3U;
    return static_cast<std::int32_t>(((state >> 16U) & 0x7fffU) %
                                     static_cast<std::uint32_t>(max_val));
}

// Retail rounding helper used in mbItem::getValue (called by 0x140247900) and
// mbGame::getScaledNumTroopsForStackTemplate (0x1400de9c0): floor(x) + (frac > 0.5f ? 1 : 0).
[[nodiscard]] inline std::int32_t retail_round_half_down(float value) noexcept {
    const float fl = std::floor(value);
    std::int32_t iv = static_cast<std::int32_t>(fl);
    if (value - fl > 0.5f) {
        ++iv;
    }
    return iv;
}

}  // namespace detail

// Cumulative level XP table from mb_warband_wse2_x64.exe (mbTroop XP + upgrade curve).
inline constexpr std::array<std::int32_t, 64> kCampaignLevelXpTable{
    0,         600,       1360,      2296,      3426,      4768,       6345,       8179,
    10297,     13010,     16161,     19806,     24007,     28832,      34362,      40682,
    47892,     56103,     65441,     77233,     90809,     106425,     124371,     144981,
    168636,    195769,    226879,    262533,    303381,    350164,     412091,     484440,
    568947,    667638,    782877,    917424,    1074494,   1257843,    1471851,    1721626,
    2070551,   2489361,   2992033,   3595340,   4319408,   5188389,    6231267,    7482821,
    8984785,   11236531,  14051314,  17569892,  21968215,  27466219,   34338823,   42929679,
    53668349,  67091786,  83871183,  160204600, 320304600, 644046000,  2050460000, 2050460000};

[[nodiscard]] inline std::int32_t troop_upgrade_xp_threshold(
    const WseTroop& troop) noexcept {
    if (troop.level < 0) {
        return 1000;
    }
    const std::size_t idx =
        static_cast<std::size_t>(std::min<std::int32_t>(troop.level + 2, 62));
    return static_cast<std::int32_t>(
        static_cast<float>(kCampaignLevelXpTable[idx]) * 0.006f + 30.0f);
}

[[nodiscard]] inline std::int32_t troop_next_level_xp(
    std::int32_t level) noexcept {
    const std::size_t idx =
        static_cast<std::size_t>(std::clamp(level + 1, 1, 62));
    return kCampaignLevelXpTable[idx];
}

// Mirrors mbTroop::getNumInventorySlots: 30 + clamp(inv_mgmt, 0, max) * 6 cargo slots.
[[nodiscard]] inline std::int32_t troop_num_cargo_slots(
    const WseWorldState& world,
    const WseTroop& troop) noexcept {
    const std::int32_t max_lvl =
        std::clamp(world.skills[kSkillInventoryManagement].max_level, 0, 15);
    const std::int32_t inv_mgmt =
        std::clamp(troop.get_skill_raw(kSkillInventoryManagement), 0, max_lvl);
    const std::int32_t cap = 30 + inv_mgmt * 6;
    return std::min(cap, kNumTroopInventorySlots);
}

[[nodiscard]] inline std::int32_t troop_total_inventory_slots(
    const WseWorldState& world,
    const WseTroop& troop) noexcept {
    return std::min(kNumTroopEquipmentSlots + troop_num_cargo_slots(world, troop),
                    kTotalTroopInventoryCapacity);
}

[[nodiscard]] inline std::int32_t troop_count_items(
    const WseWorldState& world,
    std::int32_t troop_no,
    std::int32_t item_no,
    bool equipped_only = false) noexcept {
    if (!world.is_valid_troop(troop_no)) {
        return 0;
    }
    const auto& troop = world.troops[static_cast<std::size_t>(troop_no)];
    const std::int32_t limit =
        equipped_only ? kNumTroopEquipmentSlots : troop_total_inventory_slots(world, troop);
    std::int32_t count = 0;
    for (std::int32_t i = 0; i < limit; ++i) {
        if (troop.inventory[static_cast<std::size_t>(i)].item_no == item_no) {
            ++count;
        }
    }
    return count;
}

// Computes the modifier-adjusted item value used by mbTroop::sortInventoryByValue (0x140247900).
// Safety deviation: invalid item_no returns 0 instead of indexing g_itemKinds out of bounds.
[[nodiscard]] inline std::int32_t item_modified_value(const WseWorldState& world,
                                                      const WseInventorySlot& slot) noexcept {
    if (!world.is_valid_item(slot.item_no)) {
        return 0;
    }
    const std::int32_t mod =
        (slot.modifier >= 0 && slot.modifier < kNumItemModifiers) ? slot.modifier : 0;
    const float mult = world.item_modifiers[static_cast<std::size_t>(mod)].price_multiplier;
    const float scaled =
        static_cast<float>(world.items[static_cast<std::size_t>(slot.item_no)].value) * mult;
    if (scaled >= static_cast<float>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (scaled <= static_cast<float>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return detail::retail_round_half_down(scaled);
}

// Mirrors mbItemKind::getRequirement (0x14010fdc0) with standard Warband attribute/skill defaults:
// horse(1)=Riding(24), shield(7)=Shield(26), bow(8)=PowerDraw(33), thrown(10)=PowerThrow(34),
// 1h/2h/polearm/crossbow/armor(2,3,4,9,12..15)=Strength(0), others=none(-1,-1).
inline void item_kind_requirement(const WseItemKind& item,
                                  std::int32_t& out_attr,
                                  std::int32_t& out_skill) noexcept {
    out_attr = -1;
    out_skill = -1;
    switch (item.get_type()) {
    case 1:
        out_skill = kSkillRiding;
        break;
    case 2:
    case 3:
    case 4:
    case 9:
    case 12:
    case 13:
    case 14:
    case 15:
        out_attr = 0;  // strength
        break;
    case 7:
        out_skill = kSkillShield;
        break;
    case 8:
        out_skill = kSkillPowerDraw;
        break;
    case 10:
        out_skill = kSkillPowerThrow;
        break;
    default:
        break;
    }
}

// Modifier-adjusted item difficulty read by mbTroop::canEquipItem (0x140245c60):
// returns 0 when base difficulty == 0; otherwise applies horse modifier deltas
// (mod 32 +1, mod 33 -1, mod 36 +2) and non-horse modifier deltas (mod 18 +1, mod 19 +2).
[[nodiscard]] inline std::int32_t item_effective_difficulty(const WseWorldState& world,
                                                            const WseInventorySlot& slot) noexcept {
    if (!world.is_valid_item(slot.item_no)) {
        return 0;
    }
    const auto& item = world.items[static_cast<std::size_t>(slot.item_no)];
    std::int32_t diff = item.difficulty;
    if (diff == 0) {
        return 0;
    }
    const std::int32_t mod =
        (slot.modifier >= 0 && slot.modifier < kNumItemModifiers) ? slot.modifier : 0;
    if (item.get_type() == 1) {
        if (mod == 32) {
            diff += 1;
        } else if (mod == 33) {
            diff -= 1;
        } else if (mod == 36) {
            diff += 2;
        }
    } else {
        if (mod == 18) {
            diff += 1;
        } else if (mod == 19) {
            diff += 2;
        }
    }
    return diff;
}

// Mirrors mbTroopInventory::compactSort (0x140245430): packs non-empty cargo slots forward and
// sorts [0..bag_slots) descending by item_no, breaking ties by modifier price_multiplier desc.
inline void troop_compact_inventory(const WseWorldState& world, WseTroop& troop) {
    const std::int32_t cargo_slots = troop_num_cargo_slots(world, troop);
    auto begin_it = troop.inventory.begin() + kNumTroopEquipmentSlots;
    auto end_it = begin_it + cargo_slots;
    std::int32_t write_idx = 0;
    while (write_idx < cargo_slots && !begin_it[write_idx].is_empty()) {
        ++write_idx;
    }
    for (std::int32_t read_idx = write_idx + 1; read_idx < cargo_slots; ++read_idx) {
        if (!begin_it[read_idx].is_empty()) {
            begin_it[write_idx] = begin_it[read_idx];
            begin_it[read_idx].clear();
            ++write_idx;
        }
    }
    std::sort(begin_it, end_it, [&world](const WseInventorySlot& a, const WseInventorySlot& b) {
        if (a.item_no != b.item_no) {
            return a.item_no > b.item_no;
        }
        const std::int32_t ma =
            (a.modifier >= 0 && a.modifier < kNumItemModifiers) ? a.modifier : 0;
        const std::int32_t mb =
            (b.modifier >= 0 && b.modifier < kNumItemModifiers) ? b.modifier : 0;
        return world.item_modifiers[static_cast<std::size_t>(ma)].price_multiplier >
               world.item_modifiers[static_cast<std::size_t>(mb)].price_multiplier;
    });
}

// Mirrors mbTroop::sortInventoryByValue (0x140247900): runs compactSort (0x140245430) then
// std::stable_sort by modifier-adjusted item value descending, tie-breaking by item_no ascending.
// Safety deviation: invalid item_no values are treated as value 0 and ordered after valid items.
inline void troop_sort_inventory(const WseWorldState& world, WseTroop& troop) {
    const std::int32_t cargo_slots = troop_num_cargo_slots(world, troop);
    troop_compact_inventory(world, troop);
    auto begin_it = troop.inventory.begin() + kNumTroopEquipmentSlots;
    auto end_it = begin_it + cargo_slots;
    std::stable_sort(
        begin_it, end_it, [&world](const WseInventorySlot& a, const WseInventorySlot& b) {
            const bool a_valid = world.is_valid_item(a.item_no);
            const bool b_valid = world.is_valid_item(b.item_no);
            if (a_valid != b_valid) {
                return a_valid;
            }
            if (!a_valid) {
                return false;
            }
            const std::int32_t va = item_modified_value(world, a);
            const std::int32_t vb = item_modified_value(world, b);
            if (va != vb) {
                return va > vb;
            }
            return a.item_no < b.item_no;
        });
}

// Mirrors mbGame::ensureTroopInventorySpace (0x1400e2420): counts empty cargo slots, clears
// randomly chosen non-itp_unique (bit 12 = 0x1000) items for up to 200 (0xc8) attempts until
// free_count >= required_free_slots, then calls compactSort (0x140245430).
template <typename RngFn>
inline void troop_ensure_inventory_space(const WseWorldState& world,
                                         WseTroop& troop,
                                         std::int32_t required_free_slots,
                                         RngFn&& rng) {
    const std::int32_t cargo_slots = troop_num_cargo_slots(world, troop);
    std::int32_t free_count = 0;
    for (std::int32_t i = 0; i < cargo_slots; ++i) {
        if (troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots + i)].is_empty()) {
            ++free_count;
        }
    }
    constexpr std::int32_t kMaxAttempts = 200;
    for (std::int32_t attempts = 0;
         free_count < required_free_slots && attempts < kMaxAttempts && cargo_slots > 0;
         ++attempts) {
        const std::int32_t pick = (cargo_slots > 1) ? rng(cargo_slots) : 0;
        const std::int32_t idx = std::clamp(pick, 0, cargo_slots - 1);
        auto& slot = troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots + idx)];
        if (slot.is_empty()) {
            continue;
        }
        if (world.is_valid_item(slot.item_no) &&
            ((world.items[static_cast<std::size_t>(slot.item_no)].properties >> 12U) & 1ULL) !=
                0ULL) {
            continue;  // itp_unique items are never discarded (0x1400e24e5)
        }
        slot.clear();
        ++free_count;
    }
    troop_compact_inventory(world, troop);
}

inline void troop_ensure_inventory_space(const WseWorldState& world,
                                         WseTroop& troop,
                                         std::int32_t required_free_slots) {
    std::uint32_t local_seed = 1U;
    troop_ensure_inventory_space(
        world, troop, required_free_slots,
        [&local_seed](std::int32_t m) noexcept { return detail::default_campaign_rand(local_seed, m); });
}

// Mirrors mbGame::addMerchandiseToTroop (0x1400e2520): filters items by type, !itp_unique (bit 12),
// itp_merchandise (bit 16), and faction eligibility (faction_no < 0 || item.factions.empty() ||
// faction_no in item.factions); samples `count` items weighted by abundance * probability * 0.01f
// via rglRand(15817)/15817.0f; places into getFirstFreeInventorySlot (0x1402468d0); rolls an
// optional allowed item modifier using item.modifiers + rarity_multiplier +
// merchandise_modifier_quality; and finishes with compactSort (0x140245430).
template <typename RngFn>
inline void troop_add_merchandise(WseWorldState& world,
                                  std::int32_t troop_no,
                                  std::int32_t item_type,
                                  std::int32_t count,
                                  std::int32_t faction_no,
                                  RngFn&& rng) {
    if (!world.is_valid_troop(troop_no)) {
        return;
    }
    auto& troop = world.troops[static_cast<std::size_t>(troop_no)];
    std::vector<std::int32_t> candidates;
    std::vector<float> cum_weights;
    float total_weight = 0.0f;
    for (std::int32_t item_id = 0; static_cast<std::size_t>(item_id) < world.items.size();
         ++item_id) {
        const auto& item = world.items[static_cast<std::size_t>(item_id)];
        if (item.get_type() != item_type) {
            continue;
        }
        if (((item.properties >> 12U) & 1ULL) != 0ULL) {
            continue;  // itp_unique excluded (0x1400e25fc)
        }
        if (((item.properties >> 16U) & 1ULL) == 0ULL) {
            continue;  // itp_merchandise required (0x1400e2608)
        }
        if (faction_no >= 0 && !item.factions.empty()) {
            const bool allowed =
                std::find(item.factions.begin(), item.factions.end(), faction_no) !=
                item.factions.end();
            if (!allowed) {
                continue;
            }
        }
        candidates.push_back(item_id);
        cum_weights.push_back(total_weight);
        total_weight += static_cast<float>(item.abundance) * item.probability * 0.01f;
    }

    const std::int32_t inv_max = world.skills[kSkillInventoryManagement].max_level;
    if (!candidates.empty() && count > 0) {
        for (std::int32_t n = 0; n < count; ++n) {
            const float roll = (static_cast<float>(rng(15817)) / 15817.0f) * total_weight;
            std::size_t chosen = 0;
            for (std::size_t i = 1; i < candidates.size(); ++i) {
                if (roll < cum_weights[i]) {
                    break;
                }
                chosen = i;
            }
            const std::int32_t free_slot = troop.get_first_free_inventory_slot(inv_max);
            if (free_slot < 0) {
                continue;
            }
            const std::int32_t item_id = candidates[chosen];
            const auto& item = world.items[static_cast<std::size_t>(item_id)];
            auto& slot = troop.inventory[static_cast<std::size_t>(free_slot)];
            slot.clear();
            slot.item_no = item_id;
            slot.ammo = item.max_ammo;
            slot.hit_points = item.hit_points;

            // Optional modifier roll matching 0x1400e2876 callee: 10% plain fast-path, then
            // rarity-weighted draw across bits set in item.modifiers (plus plain modifier 0).
            const float mod_gate = static_cast<float>(rng(15817)) / 15817.0f;
            if (mod_gate >= 0.1f && item.modifiers != 0ULL) {
                const float qual = world.misc.merchandise_modifier_quality;
                float bias = 1.0f;
                if (qual > 1.001f || qual < 0.999f) {
                    bias = std::fabs(qual - 1.0f) * 5.0f + 3.0f;
                }
                float mod_total = 1.0f;
                std::array<std::int32_t, static_cast<std::size_t>(kNumItemModifiers)> mods{};
                std::array<float, static_cast<std::size_t>(kNumItemModifiers)> mod_cum{};
                std::size_t mod_count = 0;
                mods[mod_count] = 0;
                mod_cum[mod_count] = 0.0f;
                ++mod_count;
                for (std::int32_t m = 1; m < kNumItemModifiers; ++m) {
                    if (((item.modifiers >> static_cast<std::uint64_t>(m)) & 1ULL) == 0ULL) {
                        continue;
                    }
                    const auto& st = world.item_modifiers[static_cast<std::size_t>(m)];
                    float w = std::max(0.0f, st.rarity_multiplier);
                    if (qual > 1.001f && st.price_multiplier > 1.0f) {
                        w *= bias;
                    } else if (qual < 0.999f && st.price_multiplier < 1.0f) {
                        w *= bias;
                    }
                    if (w > 0.0f) {
                        mods[mod_count] = m;
                        mod_cum[mod_count] = mod_total;
                        mod_total += w;
                        ++mod_count;
                    }
                }
                if (mod_count > 1U && mod_total > 0.0f) {
                    const float mroll = (static_cast<float>(rng(15817)) / 15817.0f) * mod_total;
                    std::size_t mpick = 0;
                    for (std::size_t i = 1; i < mod_count; ++i) {
                        if (mroll < mod_cum[i]) {
                            break;
                        }
                        mpick = i;
                    }
                    slot.modifier = mods[mpick];
                }
            }
        }
    }
    troop_compact_inventory(world, troop);
}

inline void troop_add_merchandise(WseWorldState& world,
                                  std::int32_t troop_no,
                                  std::int32_t item_type,
                                  std::int32_t count,
                                  std::int32_t faction_no = -1) {
    std::uint32_t local_seed = 1U;
    troop_add_merchandise(
        world, troop_no, item_type, count, faction_no,
        [&local_seed](std::int32_t m) noexcept { return detail::default_campaign_rand(local_seed, m); });
}

inline std::int32_t troop_remove_items(WseWorldState& world,
                                       std::int32_t troop_no,
                                       std::int32_t item_no,
                                       std::int32_t count) {
    if (!world.is_valid_troop(troop_no) || count <= 0) {
        return 0;
    }
    auto& troop = world.troops[static_cast<std::size_t>(troop_no)];
    const std::int32_t total = troop_total_inventory_slots(world, troop);
    std::int32_t removed = 0;
    for (std::int32_t s = kNumTroopEquipmentSlots; s < total && removed < count; ++s) {
        auto& slot = troop.inventory[static_cast<std::size_t>(s)];
        if (slot.item_no == item_no) {
            slot.clear();
            ++removed;
        }
    }
    for (std::int32_t s = 0; s < kNumTroopEquipmentSlots && removed < count; ++s) {
        auto& slot = troop.inventory[static_cast<std::size_t>(s)];
        if (slot.item_no == item_no) {
            slot.clear();
            ++removed;
        }
    }
    return removed;
}

// Mirrors mbTroop::canEquipItem (0x140245c60): compares the troop's required attribute or
// capped skill against the item's modifier-adjusted difficulty.
[[nodiscard]] inline bool troop_can_equip_item(const WseWorldState& world,
                                               const WseTroop& troop,
                                               const WseInventorySlot& slot) noexcept {
    if (!world.is_valid_item(slot.item_no)) {
        return false;
    }
    const auto& item = world.items[static_cast<std::size_t>(slot.item_no)];
    std::int32_t req_attr = -1;
    std::int32_t req_skill = -1;
    item_kind_requirement(item, req_attr, req_skill);
    std::int32_t stat_val = 0;
    if (req_attr >= 0 && req_attr < kMaxAttributes) {
        stat_val = troop.attributes[static_cast<std::size_t>(req_attr)];
    } else if (req_skill >= 0 && static_cast<std::size_t>(req_skill) < world.skills.size()) {
        const std::int32_t max_lvl =
            std::max<std::int32_t>(0, world.skills[static_cast<std::size_t>(req_skill)].max_level);
        stat_val = std::min(troop.get_skill_raw(req_skill), max_lvl);
    }
    return stat_val >= item_effective_difficulty(world, slot);
}

// Mirrors mbTroop::canEquipItemInSlot (0x140245d00): hero troops must pass canEquipItem
// (0x140245c60); slots 0..3 accept weapons/ammo/shields (2..10, 16..18), slots 4..7 accept
// armor types 12..15, and slot 8 accepts horse (1).
[[nodiscard]] inline bool troop_can_equip_item_in_slot(const WseWorldState& world,
                                                       const WseTroop& troop,
                                                       const WseInventorySlot& slot,
                                                       std::int32_t eq_slot) noexcept {
    if (!world.is_valid_item(slot.item_no) || eq_slot < 0 || eq_slot > 8) {
        return false;
    }
    if (troop.is_hero() && !troop_can_equip_item(world, troop, slot)) {
        return false;
    }
    const std::int32_t t = world.items[static_cast<std::size_t>(slot.item_no)].get_type();
    switch (eq_slot) {
    case 0:
    case 1:
    case 2:
    case 3:
        return (t >= 2 && t <= 10) || (t >= 16 && t <= 18);
    case 4:
        return t == 12;
    case 5:
        return t == 13;
    case 6:
        return t == 14;
    case 7:
        return t == 15;
    case 8:
        return t == 1;
    default:
        return false;
    }
}

// Mirrors mbTroop::unequipItem (0x140246980): moves equipment slot `eq_slot` into the first
// empty cargo slot if one exists; leaves it equipped when cargo is full.
inline void troop_unequip_item(const WseWorldState& world,
                               WseTroop& troop,
                               std::int32_t eq_slot) noexcept {
    if (eq_slot < 0 || eq_slot >= kNumTroopEquipmentSlots) {
        return;
    }
    auto& eq = troop.inventory[static_cast<std::size_t>(eq_slot)];
    if (eq.is_empty()) {
        return;
    }
    const std::int32_t inv_max = world.skills[kSkillInventoryManagement].max_level;
    const std::int32_t free_slot = troop.get_first_free_inventory_slot(inv_max);
    if (free_slot >= kNumTroopEquipmentSlots) {
        troop.inventory[static_cast<std::size_t>(free_slot)] = eq;
        eq.clear();
    }
}

// Mirrors mbTroop::unequipItems (0x140246a00): unequips slots 0..9 into cargo.
inline void troop_unequip_items(const WseWorldState& world, WseTroop& troop) noexcept {
    for (std::int32_t s = 0; s < kNumTroopEquipmentSlots; ++s) {
        troop_unequip_item(world, troop, s);
    }
}

// Mirrors mbTroop::findSuitableItem (0x140247480): scans cargo [0..bag_slots) for items
// matching canEquipItemInSlot (0x140245d00), required/excluded property masks, and up to three
// allowed item types (-1 = any). Heroes pick the highest base value; regular troops apply the
// deterministic seed hash with threshold 1.7f / (count + (guaranteed ? 0 : 3)) and fall back to
// the lowest-value candidate when `guaranteed` is true.
[[nodiscard]] inline std::int32_t troop_find_suitable_item(const WseWorldState& world,
                                                           const WseTroop& troop,
                                                           std::int32_t seed,
                                                           std::int32_t eq_slot,
                                                           std::uint32_t required_flags,
                                                           std::uint32_t excluded_flags,
                                                           std::int32_t type1,
                                                           std::int32_t type2,
                                                           std::int32_t type3,
                                                           bool guaranteed) noexcept {
    const std::int32_t cargo_slots = troop_num_cargo_slots(world, troop);
    auto matches = [&](const WseInventorySlot& s) noexcept -> bool {
        if (!world.is_valid_item(s.item_no)) {
            return false;
        }
        if (!troop_can_equip_item_in_slot(world, troop, s, eq_slot)) {
            return false;
        }
        const auto& item = world.items[static_cast<std::size_t>(s.item_no)];
        if ((item.properties & static_cast<std::uint64_t>(required_flags)) !=
            static_cast<std::uint64_t>(required_flags)) {
            return false;
        }
        if ((item.properties & static_cast<std::uint64_t>(excluded_flags)) != 0ULL) {
            return false;
        }
        if (type1 != -1) {
            const std::int32_t t = item.get_type();
            if (t != type1 && t != type2 && t != type3) {
                return false;
            }
        }
        return true;
    };

    std::int32_t match_count = 0;
    for (std::int32_t i = 0; i < cargo_slots; ++i) {
        if (matches(troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots + i)])) {
            ++match_count;
        }
    }
    if (match_count == 0) {
        return -1;
    }

    const std::int32_t denom = match_count + (guaranteed ? 0 : 3);
    const float threshold = (denom > 0) ? (1.7f / static_cast<float>(denom)) : 0.0f;

    float best_val = -1.0f;
    std::int32_t best_idx = -1;
    float fallback_val = 1.0e9f;
    std::int32_t fallback_idx = -1;

    for (std::int32_t i = 0; i < cargo_slots; ++i) {
        const auto& slot = troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots + i)];
        if (!matches(slot)) {
            continue;
        }
        const float val =
            static_cast<float>(world.items[static_cast<std::size_t>(slot.item_no)].value);
        bool take = troop.is_hero();
        if (!take) {
            const std::uint32_t h0 = static_cast<std::uint32_t>(seed) * 157U +
                                     static_cast<std::uint32_t>(slot.item_no) * 3U + 7U;
            const std::uint32_t h1 = h0 * 0x1ddcf5fU + 0x1cf5af52U;
            const float prob = static_cast<float>(h1 % 15817U) / 15817.0f;
            take = (prob < threshold);
        }
        if (take && val > best_val) {
            best_val = val;
            best_idx = i;
        }
        if (guaranteed && val < fallback_val) {
            fallback_val = val;
            fallback_idx = i;
        }
    }
    return (best_idx != -1) ? best_idx : fallback_idx;
}

// Mirrors mbTroop::equipItemSlot (0x1402473d0): finds a suitable cargo item via
// findSuitableItem (0x140247480), unequips the existing slot (0x140246980), and moves the
// chosen cargo item into `eq_slot`.
inline bool troop_equip_item_slot(const WseWorldState& world,
                                  WseTroop& troop,
                                  std::int32_t seed,
                                  std::int32_t eq_slot,
                                  std::uint32_t required_flags,
                                  std::uint32_t excluded_flags,
                                  std::int32_t type1,
                                  std::int32_t type2,
                                  std::int32_t type3,
                                  bool guaranteed) noexcept {
    if (eq_slot < 0 || eq_slot >= kNumTroopEquipmentSlots) {
        return false;
    }
    const std::int32_t cargo_idx = troop_find_suitable_item(
        world, troop, seed, eq_slot, required_flags, excluded_flags, type1, type2, type3,
        guaranteed);
    if (cargo_idx < 0) {
        return false;
    }
    troop_unequip_item(world, troop, eq_slot);
    auto& cargo = troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots + cargo_idx)];
    troop.inventory[static_cast<std::size_t>(eq_slot)] = cargo;
    cargo.clear();
    return true;
}

// Mirrors mbTroop::equipItems (0x140246a80): skips inactive troops (flag bit 5 = 0x20),
// unequips slots 0..9 into cargo (0x140246a00), equips head/body/foot/hand/horse with
// tf_guarantee_* flags (bits 22/21/20/23/24), then populates weapon slots 0..3 with
// firearms/ranged + matching ammo + shield (bit 25) + polearm (bit 27) + melee + hero fallbacks.
inline void troop_equip_items(const WseWorldState& world,
                              WseTroop& troop,
                              std::int32_t seed = -1,
                              bool check_civilian = false,
                              bool check_next_as_melee = false) {
    if ((troop.flags & 0x20ULL) != 0ULL) {
        return;  // tf_inactive (0x140246a8e)
    }
    troop_unequip_items(world, troop);

    const std::uint32_t armor_req =
        (troop.is_hero() && check_civilian) ? 0x20000000U : 0U;
    const std::uint32_t excl_flags =
        (troop.is_hero() && check_next_as_melee) ? 0x80000000U : 0U;

    const auto flag_bit = [&](std::uint32_t bit) noexcept -> bool {
        return ((troop.flags >> bit) & 1ULL) != 0ULL;
    };

    // Armor (slots 4..7) and horse (slot 8) (0x140246b1e..0x140246c87).
    (void)troop_equip_item_slot(world, troop, seed, 4, armor_req, excl_flags, -1, -1, -1,
                                flag_bit(22));  // tf_guarantee_helmet
    (void)troop_equip_item_slot(world, troop, seed, 5, armor_req, excl_flags, -1, -1, -1,
                                flag_bit(21));  // tf_guarantee_armor
    (void)troop_equip_item_slot(world, troop, seed, 6, armor_req, excl_flags, -1, -1, -1,
                                flag_bit(20));  // tf_guarantee_boots
    (void)troop_equip_item_slot(world, troop, seed, 7, armor_req, excl_flags, -1, -1, -1,
                                flag_bit(23));  // tf_guarantee_gloves
    (void)troop_equip_item_slot(world, troop, seed, 8, 0U, excl_flags, -1, -1, -1,
                                flag_bit(24));  // tf_guarantee_horse

    // Weapon / ammo / shield slots 0..3 (0x140246c8e..0x1402473c0).
    bool has_bow = false;
    bool has_xbow = false;
    bool has_firearm = false;
    bool has_arrows = false;
    bool has_bolts = false;
    bool has_bullets = false;
    bool has_shield = false;
    bool has_polearm = false;
    bool has_melee = false;
    bool needs_shield = false;

    const auto mark_shield_need = [&](std::int32_t slot_idx) noexcept {
        const std::int32_t item_no = troop.inventory[static_cast<std::size_t>(slot_idx)].item_no;
        if (world.is_valid_item(item_no) &&
            ((world.items[static_cast<std::size_t>(item_no)].properties >> 21U) & 1ULL) == 0ULL) {
            needs_shield = true;  // !itp_two_handed
        }
    };

    for (std::int32_t w = 0; w < 4; ++w) {
        bool equipped = false;
        if (w == 0) {
            const bool g_ranged = flag_bit(26);  // tf_guarantee_ranged
            equipped = troop_equip_item_slot(world, troop, seed, 0, 0U, excl_flags, 16, 17, -1,
                                             g_ranged);
            if (!equipped) {
                equipped = troop_equip_item_slot(world, troop, seed, 0, 0U, excl_flags, 8, 9, 10,
                                                 g_ranged);
            }
            if (equipped && world.is_valid_item(troop.inventory[0].item_no)) {
                const std::int32_t t =
                    world.items[static_cast<std::size_t>(troop.inventory[0].item_no)].get_type();
                if (t == 8) {
                    has_bow = true;
                } else if (t == 9) {
                    has_xbow = true;
                } else if (t == 10) {
                    needs_shield = true;
                } else if (t == 16 || t == 17) {
                    has_firearm = true;
                }
            }
        }
        if (!equipped && has_bow && !has_arrows) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 5, -1, -1, true);
            if (equipped) {
                has_arrows = true;
            }
        }
        if (!equipped && has_xbow && !has_bolts) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 6, -1, -1, true);
            if (equipped) {
                has_bolts = true;
            }
        }
        if (!equipped && has_firearm && !has_bullets) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 18, -1, -1, true);
            if (equipped) {
                has_bullets = true;
            }
        }
        if (!equipped && needs_shield && !has_shield) {
            equipped = troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 7, -1, -1,
                                             flag_bit(25));  // tf_guarantee_shield
            if (equipped) {
                has_shield = true;
            }
        }
        if (!equipped && !has_polearm) {
            equipped = troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 4, -1, -1,
                                             flag_bit(27));  // tf_guarantee_polearm
            if (equipped) {
                has_polearm = true;
                mark_shield_need(w);
            }
        }
        if (!equipped && !has_melee) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 2, 3, -1, true);
            if (equipped) {
                has_melee = true;
                mark_shield_need(w);
            } else if (!has_polearm) {
                equipped =
                    troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 4, -1, -1, true);
                if (equipped) {
                    has_polearm = true;
                    mark_shield_need(w);
                }
            }
        }
        if (!equipped) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 10, -1, -1, false);
            if (equipped) {
                needs_shield = true;
            }
        }
        if (!equipped) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 2, 3, -1, false);
            if (equipped) {
                mark_shield_need(w);
            }
        }
        if (!equipped && has_bow) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 5, -1, -1, false);
        }
        if (!equipped && has_xbow) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 6, -1, -1, false);
        }
        if (!equipped && has_firearm) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 18, -1, -1, false);
        }
        if (!equipped && troop.is_hero()) {
            equipped =
                troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 10, -1, -1, true);
            if (!equipped) {
                equipped =
                    troop_equip_item_slot(world, troop, seed, w, 0U, excl_flags, 2, 3, 4, true);
                if (equipped) {
                    mark_shield_need(w);
                }
            }
        }
    }
}

inline void troop_add_xp(WseTroop& troop, std::int32_t amount) noexcept {
    if (amount <= 0) {
        return;
    }
    const std::int32_t clamped_add = std::min<std::int32_t>(amount, 29999);
    // Safety deviation: saturate instead of wrapping past INT32_MAX.
    troop.xp = saturating_add_i32(troop.xp, clamped_add);
    while (troop.level < 62 && troop.xp >= troop_next_level_xp(troop.level)) {
        ++troop.level;
        ++troop.attribute_points;
        ++troop.skill_points;
        troop.proficiency_points += 10;
    }
}

// `rglConfig::Campaign` defaults read by `mbParty::getMaxNumCompanions` (`0x1401c18a0`) and
// `mbParty::getMaxNumPrisoners` (`0x1401c1920`): `iBaseCompanionLimit` 20,
// `iLeadershipSkillBonus` 3, `iPrisonerManagementSkillBonus` 5.
inline constexpr std::int32_t kBaseCompanionLimit = 20;
inline constexpr std::int32_t kLeadershipSkillBonus = 3;
inline constexpr std::int32_t kPrisonerManagementSkillBonus = 5;

// A troop's skill level capped by the skill's maximum (`getTroopSkill` `0x1400dcd50` without
// the modifier script).
[[nodiscard]] inline std::int32_t troop_skill_capped(const WseWorldState& world,
                                                     std::int32_t troop_no,
                                                     std::int32_t skill_no) noexcept {
    if (!world.is_valid_troop(troop_no) || skill_no < 0 ||
        static_cast<std::size_t>(skill_no) >= world.skills.size()) {
        return 0;
    }
    const std::int32_t cap =
        std::max<std::int32_t>(0, world.skills[static_cast<std::size_t>(skill_no)].max_level);
    return std::clamp(world.troops[static_cast<std::size_t>(troop_no)].get_skill_raw(skill_no), 0,
                      cap);
}

// The leader (stack 0) troop, or -1 (safety deviation: retail reads stack 0 unchecked).
[[nodiscard]] inline std::int32_t party_leader_troop(const WseWorldState& world,
                                                     const WseParty& party) noexcept {
    return (!party.stacks.empty() && world.is_valid_troop(party.stacks[0].troop_no))
               ? party.stacks[0].troop_no
               : -1;
}

// The built-in limits `getMaxNumCompanions` / `getMaxNumPrisoners` fall back to when the module
// has no `game_get_party_companion_limit` / `game_get_party_prisoner_limit` script:
// `20 + leadership * 3 + charisma` and `prisoner_management * 5`, with the leader's skills as
// `getTroopSkill` (`0x1400dcd50`) returns them. `leadership` / `prisoner_management` are those
// levels (the VM passes them with the module's skill modifier applied); -1 means "the raw level,
// capped by the skill's maximum".
[[nodiscard]] inline std::int32_t party_max_companions(const WseWorldState& world,
                                                       const WseParty& party,
                                                       std::int32_t leadership = -1) noexcept {
    const std::int32_t leader = party_leader_troop(world, party);
    if (leader < 0) {
        return kBaseCompanionLimit;
    }
    if (leadership < 0) {
        leadership = troop_skill_capped(world, leader, 1);
    }
    return kBaseCompanionLimit + leadership * kLeadershipSkillBonus +
           world.troops[static_cast<std::size_t>(leader)].attributes[3];
}

[[nodiscard]] inline std::int32_t party_max_prisoners(const WseWorldState& world,
                                                      const WseParty& party,
                                                      std::int32_t prisoner_management = -1) noexcept {
    const std::int32_t leader = party_leader_troop(world, party);
    if (leader < 0) {
        return 0;
    }
    if (prisoner_management < 0) {
        prisoner_management = troop_skill_capped(world, leader, 2);
    }
    return prisoner_management * kPrisonerManagementSkillBonus;
}

[[nodiscard]] inline std::int32_t party_count_troops(
    const WseParty& party,
    std::int32_t troop_no,
    bool prisoner) noexcept {
    std::int32_t total = 0;
    const std::uint32_t flag = prisoner ? 1U : 0U;
    for (const auto& s : party.stacks) {
        if (s.troop_no == troop_no && (s.flags & 1U) == flag) {
            total = saturating_add_i32(total, s.num_troops);
        }
    }
    return total;
}

inline std::int32_t party_add_members(WseWorldState& world,
                                      std::int32_t party_no,
                                      std::int32_t troop_no,
                                      std::int32_t count,
                                      bool prisoner = false,
                                      std::int32_t wounded = 0) {
    // Safety deviation: an invalid troop id is rejected so no stack can ever
    // carry an out-of-range troop_no.
    if (!world.is_valid_party(party_no) || !world.is_valid_troop(troop_no) || count <= 0) {
        return 0;
    }
    auto& party = world.parties[static_cast<std::size_t>(party_no)];
    const std::int32_t before = party_count_troops(party, troop_no, prisoner);
    const std::uint32_t flag = prisoner ? 1U : 0U;
    const std::int32_t clamped_wounded = std::clamp(wounded, std::int32_t{0}, count);
    WsePartyStack* target = nullptr;
    for (auto& s : party.stacks) {
        if (s.troop_no == troop_no && (s.flags & 1U) == flag) {
            target = &s;
            break;
        }
    }
    if (target != nullptr) {
        target->num_troops = saturating_add_i32(target->num_troops, count);
        target->num_wounded_troops = std::min(
            saturating_add_i32(target->num_wounded_troops, clamped_wounded),
            target->num_troops);
    } else {
        party.stacks.push_back(WsePartyStack{troop_no, count, clamped_wounded, flag});
    }
    return party_count_troops(party, troop_no, prisoner) - before;
}

inline void party_add_leader(WseWorldState& world,
                             std::int32_t party_no,
                             std::int32_t troop_no,
                             std::int32_t count = 1) {
    // Safety deviation: an invalid troop id is rejected (see party_add_members).
    if (!world.is_valid_party(party_no) || !world.is_valid_troop(troop_no) || count <= 0) {
        return;
    }
    auto& party = world.parties[static_cast<std::size_t>(party_no)];
    for (std::size_t i = 0; i < party.stacks.size(); ++i) {
        if (party.stacks[i].troop_no == troop_no && !party.stacks[i].is_prisoner()) {
            party.stacks[i].num_troops =
                saturating_add_i32(party.stacks[i].num_troops, count);
            if (i != 0) {
                std::rotate(party.stacks.begin(),
                            party.stacks.begin() + static_cast<std::ptrdiff_t>(i),
                            party.stacks.begin() + static_cast<std::ptrdiff_t>(i + 1));
            }
            return;
        }
    }
    party.stacks.insert(party.stacks.begin(), WsePartyStack{troop_no, count, 0, 0U});
}

inline std::int32_t party_remove_members(WseWorldState& world,
                                         std::int32_t party_no,
                                         std::int32_t troop_no,
                                         std::int32_t count,
                                         bool prisoner = false,
                                         bool wounded_first = false) {
    if (!world.is_valid_party(party_no) || count <= 0) {
        return 0;
    }
    auto& party = world.parties[static_cast<std::size_t>(party_no)];
    const std::uint32_t flag = prisoner ? 1U : 0U;
    for (std::size_t idx = party.stacks.size(); idx > 0; --idx) {
        auto& s = party.stacks[idx - 1];
        if (s.troop_no == troop_no && (s.flags & 1U) == flag) {
            const std::int32_t removed = std::min(count, s.num_troops);
            if (wounded_first) {
                const std::int32_t wounded_removed = std::min(removed, s.num_wounded_troops);
                s.num_wounded_troops -= wounded_removed;
            }
            s.num_troops -= removed;
            if (s.num_wounded_troops > s.num_troops) {
                s.num_wounded_troops = s.num_troops;
            }
            if (s.num_upgradeable > s.num_troops) {
                s.num_upgradeable = s.num_troops;
            }
            if (s.num_troops <= 0) {
                party.stacks.erase(party.stacks.begin() + static_cast<std::ptrdiff_t>(idx - 1));
            }
            return removed;
        }
    }
    return 0;
}

inline std::int32_t party_wound_members(WseParty& party,
                                        std::int32_t troop_no,
                                        std::int32_t count) noexcept {
    if (count <= 0) {
        return 0;
    }
    std::int32_t remaining = count;
    std::int32_t wounded_total = 0;
    for (auto& s : party.stacks) {
        if (s.troop_no == troop_no && !s.is_prisoner() && remaining > 0) {
            const std::int32_t avail = s.unwounded();
            const std::int32_t delta = std::min(avail, remaining);
            s.num_wounded_troops += delta;
            wounded_total += delta;
            remaining -= delta;
        }
    }
    return wounded_total;
}

inline std::int32_t party_heal_members(WseParty& party,
                                       std::int32_t troop_no,
                                       std::int32_t count) noexcept {
    if (count <= 0) {
        return 0;
    }
    std::int32_t remaining = count;
    std::int32_t healed_total = 0;
    for (auto& s : party.stacks) {
        if (s.troop_no == troop_no && !s.is_prisoner() && remaining > 0) {
            const std::int32_t delta = std::min(s.num_wounded_troops, remaining);
            s.num_wounded_troops -= delta;
            healed_total += delta;
            remaining -= delta;
        }
    }
    return healed_total;
}

inline bool party_switch_stacks(WseParty& party,
                                std::int32_t stack_no_1,
                                std::int32_t stack_no_2) noexcept {
    if (stack_no_1 < 0 || stack_no_2 < 0 ||
        static_cast<std::size_t>(stack_no_1) >= party.stacks.size() ||
        static_cast<std::size_t>(stack_no_2) >= party.stacks.size()) {
        return false;
    }
    if (stack_no_1 != stack_no_2) {
        std::swap(party.stacks[static_cast<std::size_t>(stack_no_1)],
                  party.stacks[static_cast<std::size_t>(stack_no_2)]);
    }
    return true;
}

// Mirrors mbParty::addExperienceToStack (0x1401c2d30) with optional VM-supplied
// mbGame::getUpgradeExperience (0x1400e05f0) threshold.
inline void party_add_xp_to_stack(WseWorldState& world,
                                  std::int32_t party_no,
                                  std::int32_t stack_no,
                                  std::int32_t xp_amount,
                                  std::int32_t upgrade_xp_override = -1) noexcept {
    if (!world.is_valid_party(party_no) || stack_no < 0 || xp_amount <= 0) {
        return;
    }
    auto& party = world.parties[static_cast<std::size_t>(party_no)];
    if (static_cast<std::size_t>(stack_no) >= party.stacks.size()) {
        return;
    }
    auto& st = party.stacks[static_cast<std::size_t>(stack_no)];
    if (!world.is_valid_troop(st.troop_no)) {
        return;
    }
    auto& troop = world.troops[static_cast<std::size_t>(st.troop_no)];
    if (troop.is_hero()) {
        troop_add_xp(troop, xp_amount);
        return;
    }
    if (troop.upgrade_troop_nos[0] <= 0) {
        return;
    }
    st.xp += static_cast<float>(xp_amount);
    const std::int32_t req_i =
        (upgrade_xp_override >= 0) ? upgrade_xp_override : troop_upgrade_xp_threshold(troop);
    const float req = static_cast<float>(req_i);
    if (req <= 0.0f) {
        return;
    }
    while (st.xp >= req && st.num_upgradeable < st.num_troops) {
        st.xp -= req;
        ++st.num_upgradeable;
    }
    if (st.num_upgradeable >= st.num_troops) {
        st.xp = 0.0f;
    }
}

inline bool party_stack_upgrade(WseWorldState& world,
                                std::int32_t party_no,
                                std::int32_t stack_no,
                                std::int32_t amount,
                                std::int32_t upgrade_path) {
    if (!world.is_valid_party(party_no) || stack_no < 0 || amount <= 0 ||
        upgrade_path < 0 || upgrade_path > 1) {
        return false;
    }
    auto& party = world.parties[static_cast<std::size_t>(party_no)];
    if (static_cast<std::size_t>(stack_no) >= party.stacks.size()) {
        return false;
    }
    const auto st_copy = party.stacks[static_cast<std::size_t>(stack_no)];
    if (st_copy.is_prisoner() || !world.is_valid_troop(st_copy.troop_no)) {
        return false;
    }
    const std::int32_t target_troop =
        world.troops[static_cast<std::size_t>(st_copy.troop_no)]
            .upgrade_troop_nos[static_cast<std::size_t>(upgrade_path)];
    if (!world.is_valid_troop(target_troop)) {
        return false;
    }
    const std::int32_t upg =
        std::min({amount, st_copy.num_upgradeable, st_copy.num_troops});
    if (upg <= 0) {
        return false;
    }
    // Mirrors mbGame::upgradeParty (0x1400df513..0x1400df52b): wounded troops carried over =
    // max(0, upg - (num_troops - num_wounded)).
    const std::int32_t carried_wounded =
        std::max<std::int32_t>(0, upg - (st_copy.num_troops - st_copy.num_wounded_troops));
    auto& st = party.stacks[static_cast<std::size_t>(stack_no)];
    st.num_upgradeable -= upg;
    st.num_troops -= upg;
    if (st.num_wounded_troops > st.num_troops) {
        st.num_wounded_troops = st.num_troops;
    }
    if (st.num_troops <= 0) {
        party.stacks.erase(party.stacks.begin() + static_cast<std::ptrdiff_t>(stack_no));
    }
    party_add_members(world, party_no, target_troop, upg, false, carried_wounded);
    return true;
}

// Mirrors mbGame::addExperienceToParty (0x1400e64e0): weights leader hero = 10, other heroes = 3,
// regular stacks = num_troops, stopping at the first prisoner stack.
template <typename UpgradeXpFn>
inline void party_add_xp(WseWorldState& world,
                         std::int32_t party_no,
                         std::int32_t total_xp,
                         UpgradeXpFn&& upgrade_xp_fn) {
    if (!world.is_valid_party(party_no) || total_xp <= 0) {
        return;
    }
    const auto& party = world.parties[static_cast<std::size_t>(party_no)];
    std::int32_t denom = 0;
    for (std::size_t i = 0; i < party.stacks.size(); ++i) {
        const auto& st = party.stacks[i];
        if (st.is_prisoner()) {
            break;
        }
        if (world.is_valid_troop(st.troop_no) &&
            world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
            denom += (i == 0 ? 10 : 3);
        } else {
            denom += st.num_troops;
        }
    }
    if (denom <= 0) {
        return;
    }
    const float xp_per_share = static_cast<float>(total_xp) / static_cast<float>(denom);
    const std::size_t n = party.stacks.size();
    for (std::size_t i = 0; i < n; ++i) {
        const auto st_copy = world.parties[static_cast<std::size_t>(party_no)].stacks[i];
        if (st_copy.is_prisoner()) {
            break;
        }
        if (world.is_valid_troop(st_copy.troop_no) &&
            world.troops[static_cast<std::size_t>(st_copy.troop_no)].is_hero()) {
            const float share = (i == 0) ? (xp_per_share * 10.0f) : (xp_per_share * 3.0f);
            party_add_xp_to_stack(world, party_no, static_cast<std::int32_t>(i),
                                  static_cast<std::int32_t>(share), -1);
        } else {
            const float share = static_cast<float>(st_copy.num_troops) * xp_per_share;
            const bool can_upg =
                world.is_valid_troop(st_copy.troop_no) &&
                world.troops[static_cast<std::size_t>(st_copy.troop_no)].upgrade_troop_nos[0] > 0;
            const std::int32_t req = can_upg ? upgrade_xp_fn(st_copy.troop_no) : -1;
            party_add_xp_to_stack(world, party_no, static_cast<std::int32_t>(i),
                                  static_cast<std::int32_t>(share), req);
        }
    }
}

inline void party_add_xp(WseWorldState& world,
                         std::int32_t party_no,
                         std::int32_t total_xp) noexcept {
    party_add_xp(world, party_no, total_xp, [](std::int32_t) noexcept { return -1; });
}

// Mirrors mbGame::upgradeParty (0x1400df0e0): upgrade_path_choice 0 picks random path 0 or 1
// (rglRand & 1), 1 selects path 0, 2 selects path 1 (falling back to path 0 when slot 1 is
// absent); distributes XP with per-stack jitter [0.75, 1.25) (0.75f + 0.5f * rglRand(15817) /
// 15817.0f) and hero weight 10 vs 3; then upgrades all upgradeable regular stacks, carrying
// wounded counts over. The two hero weights differ on purpose, as in retail: the total counts
// 10 for the player troop (0x1400df232, `cmp ebx, [game+0x42c]`), the shares give 10 to stack 0
// (0x1400df366, `test ebx, ebx`). In a party the player does not lead, the shares do not add up
// to `total_xp`. (party_add_xp, 0x1400e64e0, uses stack 0 in both.)
template <typename RngFn, typename UpgradeXpFn>
inline void party_upgrade_with_xp(WseWorldState& world,
                                  std::int32_t party_no,
                                  std::int32_t total_xp,
                                  std::int32_t upgrade_path_choice,
                                  RngFn&& rng,
                                  UpgradeXpFn&& upgrade_xp_fn) {
    if (!world.is_valid_party(party_no) || total_xp <= 0 || upgrade_path_choice < 0 ||
        upgrade_path_choice > 2) {
        return;
    }
    const std::int32_t chosen_path =
        (upgrade_path_choice == 0) ? (rng(2) & 1) : (upgrade_path_choice - 1);

    const auto& party_pre = world.parties[static_cast<std::size_t>(party_no)];
    std::int32_t denom = 0;
    for (std::size_t i = 0; i < party_pre.stacks.size(); ++i) {
        const auto& st = party_pre.stacks[i];
        if (st.is_prisoner()) {
            break;
        }
        if (world.is_valid_troop(st.troop_no) &&
            world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
            denom += (st.troop_no == world.player_troop_no) ? 10 : 3;
        } else {
            denom += st.num_troops;
        }
    }
    if (denom > 0) {
        const float xp_per_share = static_cast<float>(total_xp) / static_cast<float>(denom);
        const std::size_t n = party_pre.stacks.size();
        for (std::size_t i = 0; i < n; ++i) {
            const auto st_copy = world.parties[static_cast<std::size_t>(party_no)].stacks[i];
            if (st_copy.is_prisoner()) {
                break;
            }
            const float jitter =
                0.75f + 0.5f * (static_cast<float>(rng(15817)) / 15817.0f);
            if (world.is_valid_troop(st_copy.troop_no) &&
                world.troops[static_cast<std::size_t>(st_copy.troop_no)].is_hero()) {
                const float w = (i == 0) ? 10.0f : 3.0f;
                const auto gain = static_cast<std::int32_t>(xp_per_share * w * jitter);
                party_add_xp_to_stack(world, party_no, static_cast<std::int32_t>(i), gain, -1);
            } else {
                const auto gain = static_cast<std::int32_t>(
                    static_cast<float>(st_copy.num_troops) * xp_per_share * jitter);
                const bool can_upg =
                    world.is_valid_troop(st_copy.troop_no) &&
                    world.troops[static_cast<std::size_t>(st_copy.troop_no)].upgrade_troop_nos[0] >
                        0;
                const std::int32_t req = can_upg ? upgrade_xp_fn(st_copy.troop_no) : -1;
                party_add_xp_to_stack(world, party_no, static_cast<std::int32_t>(i), gain, req);
            }
        }
    }

    const auto& party_ref = world.parties[static_cast<std::size_t>(party_no)];
    struct PendingUpgrade {
        std::int32_t src_troop;
        std::int32_t path;
        std::int32_t count;
    };
    std::vector<PendingUpgrade> upgrades;
    for (const auto& st : party_ref.stacks) {
        if (st.is_prisoner()) {
            break;
        }
        if (st.num_upgradeable <= 0 || !world.is_valid_troop(st.troop_no)) {
            continue;
        }
        const auto& tr = world.troops[static_cast<std::size_t>(st.troop_no)];
        if (tr.is_hero()) {
            continue;
        }
        const std::int32_t path =
            (chosen_path == 1 && tr.upgrade_troop_nos[1] > 0) ? 1 : 0;
        const std::int32_t dst = tr.upgrade_troop_nos[static_cast<std::size_t>(path)];
        if (dst > 0 && world.is_valid_troop(dst)) {
            upgrades.push_back(PendingUpgrade{st.troop_no, path, st.num_upgradeable});
        }
    }
    for (const auto& u : upgrades) {
        auto& stacks = world.parties[static_cast<std::size_t>(party_no)].stacks;
        for (std::size_t i = 0; i < stacks.size(); ++i) {
            if (!stacks[i].is_prisoner() && stacks[i].troop_no == u.src_troop) {
                party_stack_upgrade(world, party_no, static_cast<std::int32_t>(i), u.count, u.path);
                break;
            }
        }
    }
}

inline void party_upgrade_with_xp(WseWorldState& world,
                                  std::int32_t party_no,
                                  std::int32_t total_xp,
                                  std::int32_t upgrade_path_choice) {
    std::uint32_t local_seed = 1U;
    party_upgrade_with_xp(
        world, party_no, total_xp, upgrade_path_choice,
        [&local_seed](std::int32_t m) noexcept { return detail::default_campaign_rand(local_seed, m); },
        [](std::int32_t) noexcept { return -1; });
}

// Mirrors mbGame::getScaledNumTroopsForStackTemplate (0x1400de9c0) + mbGame::addTemplateToParty
// (0x1400de710): scales each stack count between [min_troops, max_troops] using
// world.party_creation_min_random / world.party_creation_max_random (set by 1080
// set_party_creation_random_limits, 0x1401cd794) and rglRand(15817)/15817.0f with
// retail half-down rounding.
template <typename RngFn>
inline void party_add_template(WseWorldState& world,
                               std::int32_t party_no,
                               std::int32_t template_no,
                               bool reverse_prisoner,
                               RngFn&& rng) {
    if (!world.is_valid_party(party_no) || !world.is_valid_party_template(template_no)) {
        return;
    }
    const auto& tpl = world.party_templates[static_cast<std::size_t>(template_no)];
    const float min_pct = static_cast<float>(world.party_creation_min_random);
    const float span_pct =
        static_cast<float>(world.party_creation_max_random - world.party_creation_min_random);
    for (const auto& st : tpl.stacks) {
        if (st.troop_no < 0) {
            continue;
        }
        const float r = static_cast<float>(rng(15817)) / 15817.0f;
        const float pct = min_pct + span_pct * r;
        const float delta =
            static_cast<float>(st.max_troops - st.min_troops) * pct * 0.01f;
        const std::int32_t n = st.min_troops + detail::retail_round_half_down(delta);
        if (n <= 0) {
            continue;
        }
        const bool is_prisoner = ((st.flags & 1U) != 0U) != reverse_prisoner;
        party_add_members(world, party_no, st.troop_no, n, is_prisoner, 0);
    }
}

inline void party_add_template(WseWorldState& world,
                               std::int32_t party_no,
                               std::int32_t template_no,
                               bool reverse_prisoner = false) {
    std::uint32_t local_seed = 1U;
    party_add_template(
        world, party_no, template_no, reverse_prisoner,
        [&local_seed](std::int32_t m) noexcept { return detail::default_campaign_rand(local_seed, m); });
}

// `max_companions` / `max_prisoners` are the destination's limits (the VM passes the module
// script's values); -1 uses the built-in fallback.
[[nodiscard]] inline bool party_can_join_party(const WseWorldState& world,
                                               std::int32_t src_party_no,
                                               std::int32_t dst_party_no,
                                               bool as_prisoners = false,
                                               std::int32_t max_companions = -1,
                                               std::int32_t max_prisoners = -1) noexcept {
    if (!world.is_valid_party(src_party_no) || !world.is_valid_party(dst_party_no)) {
        return false;
    }
    const auto& src = world.parties[static_cast<std::size_t>(src_party_no)];
    const auto& dst = world.parties[static_cast<std::size_t>(dst_party_no)];
    if (max_companions < 0) {
        max_companions = party_max_companions(world, dst);
    }
    if (max_prisoners < 0) {
        max_prisoners = party_max_prisoners(world, dst);
    }
    if (as_prisoners) {
        const std::int32_t incoming = src.get_num_companions() + src.get_num_prisoners();
        return dst.get_num_prisoners() + incoming <= max_prisoners;
    }
    const std::int32_t free_comp = max_companions - dst.get_num_companions();
    const std::int32_t free_pris = max_prisoners - dst.get_num_prisoners();
    return src.get_num_companions() <= free_comp && src.get_num_prisoners() <= free_pris;
}

inline void party_join_to_party(WseWorldState& world,
                                std::int32_t dst_party_no,
                                std::int32_t src_party_no,
                                bool as_prisoners = false) {
    if (!world.is_valid_party(dst_party_no) || !world.is_valid_party(src_party_no) ||
        dst_party_no == src_party_no) {
        return;
    }
    const auto src_stacks = world.parties[static_cast<std::size_t>(src_party_no)].stacks;
    for (const auto& st : src_stacks) {
        if (st.num_troops <= 0) {
            continue;
        }
        const bool pris = as_prisoners ? true : st.is_prisoner();
        party_add_members(world, dst_party_no, st.troop_no, st.num_troops, pris,
                          st.num_wounded_troops);
    }
    if (src_party_no != world.main_party_no) {
        (void)world.remove_party(src_party_no);
    } else {
        world.parties[static_cast<std::size_t>(src_party_no)].stacks.clear();
    }
}

inline void party_collect_attachments(WseWorldState& world,
                                      std::int32_t src_party_no,
                                      std::int32_t dst_party_no) {
    if (!world.is_valid_party(src_party_no) || !world.is_valid_party(dst_party_no)) {
        return;
    }
    const auto attached =
        world.parties[static_cast<std::size_t>(src_party_no)].attached_party_nos;
    for (const std::int32_t att_no : attached) {
        if (!world.is_valid_party(att_no) || att_no == dst_party_no) {
            continue;
        }
        const auto stacks = world.parties[static_cast<std::size_t>(att_no)].stacks;
        for (const auto& st : stacks) {
            if (st.num_troops > 0) {
                party_add_members(world, dst_party_no, st.troop_no, st.num_troops,
                                  st.is_prisoner(), st.num_wounded_troops);
            }
        }
    }
}

[[nodiscard]] inline bool check_entering_town(std::int32_t town_party_no,
                                              const WseWorldState& world) noexcept {
    if (!world.is_valid_party(world.main_party_no) || !world.is_valid_party(town_party_no)) {
        return false;
    }
    const auto& main_p = world.parties[static_cast<std::size_t>(world.main_party_no)];
    if (main_p.behavior != 4 || main_p.behavior_object != town_party_no) {
        return false;
    }
    const auto& town_p = world.parties[static_cast<std::size_t>(town_party_no)];
    const float dx = town_p.position.o.x - main_p.position.o.x;
    const float dy = town_p.position.o.y - main_p.position.o.y;
    const float dz = town_p.position.o.z - main_p.position.o.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    return dist > main_p.radius * 2.3f;
}

[[nodiscard]] inline std::int32_t party_count_troop_type(const WseWorldState& world,
                                                         std::int32_t party_no,
                                                         std::int32_t troop_no,
                                                         bool prisoner) noexcept {
    if (!world.is_valid_party(party_no)) {
        return 0;
    }
    std::int32_t total = 0;
    for (const auto& st : world.parties[static_cast<std::size_t>(party_no)].stacks) {
        if (st.troop_no == troop_no && st.is_prisoner() == prisoner) {
            total = saturating_add_i32(total, st.num_troops);
        }
    }
    return total;
}

[[nodiscard]] inline std::int32_t troop_free_inventory_capacity(
    const WseWorldState& world,
    std::int32_t troop_no) noexcept {
    if (!world.is_valid_troop(troop_no)) {
        return 0;
    }
    const auto& troop = world.troops[static_cast<std::size_t>(troop_no)];
    const std::int32_t slots = troop_num_cargo_slots(world, troop);
    std::int32_t free_count = 0;
    for (std::int32_t i = 0; i < slots; ++i) {
        const auto idx = static_cast<std::size_t>(kNumTroopEquipmentSlots + i);
        if (idx < troop.inventory.size() && troop.inventory[idx].item_no == -1) {
            ++free_count;
        }
    }
    return free_count;
}

inline void party_attach_to_current_battle(WseWorldState& world,
                                           std::int32_t party_no,
                                           std::int32_t side) {
    if (!world.detach_party(party_no)) {
        return;
    }
    world.parties[static_cast<std::size_t>(party_no)].in_battle = true;
    std::int32_t target_no = world.main_party_no;
    if (side == 1) {
        target_no = world.encountered_party_no;
    } else if (side == 2) {
        target_no = world.encountered_party_2_no;
    }
    (void)world.attach_party(party_no, target_no);
}

inline void party_leave_cur_battle(WseWorldState& world, std::int32_t party_no) {
    if (!world.is_valid_party(party_no)) {
        return;
    }
    auto& p = world.parties[static_cast<std::size_t>(party_no)];
    const bool had_parent = world.is_valid_party(p.attached_to_party_no);
    p.in_battle = false;
    (void)world.detach_party(party_no);
    if (!had_parent && party_no == world.main_party_no) {
        world.misc.meta_mission_event_ended = true;
    }
}

[[nodiscard]] inline std::vector<std::int32_t> party_group_members(
    const WseWorldState& world,
    std::int32_t leader_party_no) {
    std::vector<std::int32_t> group;
    if (!world.is_valid_party(leader_party_no)) {
        return group;
    }
    group.push_back(leader_party_no);
    for (const std::int32_t att :
         world.parties[static_cast<std::size_t>(leader_party_no)].attached_party_nos) {
        if (world.is_valid_party(att) &&
            std::find(group.begin(), group.end(), att) == group.end()) {
            group.push_back(att);
        }
    }
    return group;
}

inline void party_inflict_casualties_group(WseWorldState& world,
                                           std::int32_t leader_party_no,
                                           std::int32_t num_casualties,
                                           std::int32_t casualties_party_no,
                                           std::uint32_t seed) {
    if (num_casualties <= 0) {
        return;
    }
    const auto group = party_group_members(world, leader_party_no);
    if (group.empty()) {
        return;
    }
    const bool record_casualties =
        world.is_valid_party(casualties_party_no) &&
        std::find(group.begin(), group.end(), casualties_party_no) == group.end();
    auto count_able = [&]() -> std::int64_t {
        std::int64_t total = 0;
        for (const std::int32_t pid : group) {
            for (const auto& st : world.parties[static_cast<std::size_t>(pid)].stacks) {
                if (st.is_prisoner() || !world.is_valid_troop(st.troop_no)) {
                    continue;
                }
                if (world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
                    if (world.troops[static_cast<std::size_t>(st.troop_no)].hit_points > 0.0f) {
                        ++total;
                    }
                } else {
                    total += st.unwounded();
                }
            }
        }
        return total;
    };
    const std::int64_t cap = count_able();
    const std::int64_t hits = std::min<std::int64_t>(num_casualties, cap);
    std::uint32_t rng = seed ^ 0x9e3779b9U;
    for (std::int64_t i = 0; i < hits; ++i) {
        const std::int64_t rem = count_able();
        if (rem <= 0) {
            break;
        }
        rng = rng * 1664525U + 1013904223U;
        std::int64_t target = static_cast<std::int64_t>(rng % static_cast<std::uint64_t>(rem));
        bool applied = false;
        for (const std::int32_t pid : group) {
            auto& stacks = world.parties[static_cast<std::size_t>(pid)].stacks;
            for (std::size_t s = 0; s < stacks.size(); ++s) {
                auto& st = stacks[s];
                if (st.is_prisoner() || !world.is_valid_troop(st.troop_no)) {
                    continue;
                }
                auto& troop = world.troops[static_cast<std::size_t>(st.troop_no)];
                const std::int64_t w =
                    troop.is_hero() ? (troop.hit_points > 0.0f ? 1 : 0) : st.unwounded();
                if (target >= w) {
                    target -= w;
                    continue;
                }
                if (troop.is_hero()) {
                    troop.hit_points = std::max(0.0f, troop.hit_points - 15.0f);
                } else {
                    const std::int32_t tno = st.troop_no;
                    rng = rng * 1664525U + 1013904223U;
                    const bool kill = (rng % 100U) < 35U;
                    if (kill) {
                        --st.num_troops;
                        if (st.num_wounded_troops > st.num_troops) {
                            st.num_wounded_troops = st.num_troops;
                        }
                        if (st.num_troops <= 0) {
                            stacks.erase(stacks.begin() + static_cast<std::ptrdiff_t>(s));
                        }
                    } else {
                        ++st.num_wounded_troops;
                    }
                    if (record_casualties) {
                        party_add_members(world, casualties_party_no, tno, 1, false,
                                          kill ? 0 : 1);
                    }
                }
                applied = true;
                break;
            }
            if (applied) {
                break;
            }
        }
    }
}

inline void party_distribute_among_group(WseWorldState& world,
                                         std::int32_t src_party_no,
                                         std::int32_t dst_leader_party_no) {
    if (!world.is_valid_party(src_party_no) || src_party_no == dst_leader_party_no) {
        return;
    }
    const auto group = party_group_members(world, dst_leader_party_no);
    if (group.empty()) {
        return;
    }
    std::int64_t total_weight = 0;
    for (const std::int32_t pid : group) {
        total_weight += std::max<std::int32_t>(
            1, world.parties[static_cast<std::size_t>(pid)].get_num_companions());
    }
    auto& src_stacks = world.parties[static_cast<std::size_t>(src_party_no)].stacks;
    std::vector<WsePartyStack> kept;
    for (const auto& st : src_stacks) {
        if (st.is_prisoner() || !world.is_valid_troop(st.troop_no) ||
            world.troops[static_cast<std::size_t>(st.troop_no)].is_hero() ||
            st.num_troops <= 0) {
            kept.push_back(st);
            continue;
        }
        std::int32_t rem_t = st.num_troops;
        std::int32_t rem_w = std::clamp(st.num_wounded_troops, 0, st.num_troops);
        for (std::size_t idx = 0; idx < group.size() && rem_t > 0; ++idx) {
            const std::int32_t pid = group[idx];
            const std::int64_t w = std::max<std::int32_t>(
                1, world.parties[static_cast<std::size_t>(pid)].get_num_companions());
            const std::int32_t share =
                (idx + 1 == group.size())
                    ? rem_t
                    : static_cast<std::int32_t>(
                          std::min<std::int64_t>(rem_t, (static_cast<std::int64_t>(st.num_troops) * w) / total_weight));
            if (share > 0) {
                const std::int32_t w_share = std::min(rem_w, share);
                party_add_members(world, pid, st.troop_no, share, false, w_share);
                rem_t -= share;
                rem_w -= w_share;
            }
        }
    }
    src_stacks = std::move(kept);
}

[[nodiscard]] inline std::int32_t mission_count_normalized_team_members(
    const WseWorldState& world,
    float battle_size,
    std::int32_t team_no) noexcept {
    if (!world.is_valid_team(team_no)) {
        return 0;
    }
    std::array<std::int32_t, kNumTeams> alive_per_team{};
    for (const auto& ag : world.agents) {
        if (ag.is_alive() && world.is_valid_team(ag.team_no)) {
            ++alive_per_team[static_cast<std::size_t>(ag.team_no)];
        }
    }
    std::int32_t count = 0;
    for (std::int32_t t = 0; t < kNumTeams; ++t) {
        if (world.teams[static_cast<std::size_t>(team_no)]
                .relations[static_cast<std::size_t>(t)] > 0) {
            count += alive_per_team[static_cast<std::size_t>(t)];
        }
    }
    if (count == 0 || !world.is_valid_agent(world.player_agent_no)) {
        return 0;
    }
    const std::int32_t player_team =
        world.agents[static_cast<std::size_t>(world.player_agent_no)].team_no;
    if (!world.is_valid_team(player_team)) {
        return 0;
    }
    std::int32_t adv = world.battle_advantage;
    if (world.teams[static_cast<std::size_t>(team_no)]
            .relations[static_cast<std::size_t>(player_team)] < 0) {
        adv = -adv;
    }
    const float adv_mult = std::clamp((static_cast<float>(adv) + 15.0f) / 15.0f, 0.2f, 2.5f);
    const float denom = (battle_size * 4.0f + 1.25f) * adv_mult;
    if (denom <= 0.0f) {
        return std::max<std::int32_t>(1, count);
    }
    const float ratio = static_cast<float>(count) / denom;
    const float fl = std::floor(ratio);
    const std::int32_t rounded =
        static_cast<std::int32_t>(fl) + ((ratio - fl > 0.5f) ? 1 : 0);
    return std::max<std::int32_t>(1, rounded);
}

inline void mission_add_reinforcements(WseWorldState& world,
                                       std::int32_t entry_group_no,
                                       std::int32_t num_troops) {
    if (entry_group_no >= 0 && entry_group_no < kMaxEntryPoints) {
        world.entry_points[static_cast<std::size_t>(entry_group_no)].num_x_moves = 0;
        world.entry_points[static_cast<std::size_t>(entry_group_no)].num_y_moves = 0;
    }
    push_bounded(world.fx.reinforcement_events,
                 WseReinforcementEvent{entry_group_no, num_troops},
                 kMaxReinforcementEvents);
}

inline void mission_burst_particle_system(WseWorldState& world,
                                          std::int32_t psys_no,
                                          const wse::core::WseTransform& transform,
                                          std::int32_t burst_strength,
                                          bool synced) {
    if (!world.is_valid_particle_system(psys_no)) {
        return;
    }
    push_bounded(world.fx.particle_events,
                 WseParticleSystemEntry{psys_no, 2, 0, transform, burst_strength, 0, synced},
                 kMaxParticleEvents);
}

[[nodiscard]] inline std::int32_t mission_spawn_item(WseWorldState& world,
                                                     std::int32_t item_no,
                                                     std::int32_t modifier,
                                                     bool refill_ammo,
                                                     std::int32_t prune_seconds = 0) {
    if (!world.is_valid_item(item_no) || !world.lighting.has_scene) {
        return -1;
    }
    WseMissionObject obj{};
    obj.active = true;
    obj.is_item = true;
    obj.m5.is_spawned_item = true;
    if (prune_seconds > 0) {
        obj.m5.enable_pruning = true;
        obj.m5.prune_time = static_cast<float>(prune_seconds);
    }
    obj.scene_prop_no = item_no;
    obj.variation_id_no = (modifier >= 0 && modifier < 43) ? modifier : 0;
    obj.variation_id_2_no = refill_ammo ? 1 : 0;
    obj.position = world.spawn_position;
    obj.starting_position = world.spawn_position;
    obj.target_position = world.spawn_position;
    return world.allocate_mission_object(std::move(obj));
}

}  // namespace wse::game
