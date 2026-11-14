// Campaign and inventory helpers for WseWorldState, WseParty, and WseTroop.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "wse/game/world_state.h"

namespace wse::game {

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

inline void troop_sort_inventory(const WseWorldState& world, WseTroop& troop) {
    const std::int32_t cargo_slots = troop_num_cargo_slots(world, troop);
    auto begin_it = troop.inventory.begin() + kNumTroopEquipmentSlots;
    auto end_it = begin_it + cargo_slots;
    std::sort(begin_it, end_it, [&world](const WseInventorySlot& a, const WseInventorySlot& b) {
        const bool a_valid = world.is_valid_item(a.item_no);
        const bool b_valid = world.is_valid_item(b.item_no);
        if (a_valid != b_valid) {
            return a_valid;
        }
        if (!a_valid) {
            return false;
        }
        const auto va = world.items[static_cast<std::size_t>(a.item_no)].value;
        const auto vb = world.items[static_cast<std::size_t>(b.item_no)].value;
        if (va != vb) {
            return va > vb;
        }
        return a.item_no < b.item_no;
    });
}

inline void troop_ensure_inventory_space(const WseWorldState& world,
                                         WseTroop& troop,
                                         std::int32_t required_free_slots) {
    if (required_free_slots <= 0) {
        return;
    }
    const std::int32_t cargo_slots = troop_num_cargo_slots(world, troop);
    auto begin_it = troop.inventory.begin() + kNumTroopEquipmentSlots;
    auto end_it = begin_it + cargo_slots;
    std::stable_partition(begin_it, end_it, [](const WseInventorySlot& s) {
        return !s.is_empty();
    });
    std::int32_t free_count = 0;
    for (std::int32_t i = 0; i < cargo_slots; ++i) {
        if (troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots + i)].is_empty()) {
            ++free_count;
        }
    }
    while (free_count < required_free_slots) {
        std::int32_t cheapest_idx = -1;
        std::int32_t cheapest_val = 0;
        for (std::int32_t i = 0; i < cargo_slots; ++i) {
            const auto& slot =
                troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots + i)];
            if (slot.is_empty()) {
                continue;
            }
            const std::int32_t val =
                world.is_valid_item(slot.item_no)
                    ? world.items[static_cast<std::size_t>(slot.item_no)].value
                    : 0;
            if (cheapest_idx < 0 || val < cheapest_val) {
                cheapest_idx = kNumTroopEquipmentSlots + i;
                cheapest_val = val;
            }
        }
        if (cheapest_idx < 0) {
            break;
        }
        troop.inventory[static_cast<std::size_t>(cheapest_idx)].clear();
        ++free_count;
    }
}

inline void troop_add_merchandise(WseWorldState& world,
                                  std::int32_t troop_no,
                                  std::int32_t item_type,
                                  std::int32_t count,
                                  std::int32_t faction_no = -1) {
    (void)faction_no;
    if (!world.is_valid_troop(troop_no) || count <= 0) {
        return;
    }
    auto& troop = world.troops[static_cast<std::size_t>(troop_no)];
    const std::int32_t total = troop_total_inventory_slots(world, troop);
    std::vector<std::int32_t> candidates;
    for (std::int32_t item_id = 0; static_cast<std::size_t>(item_id) < world.items.size();
         ++item_id) {
        const auto& item = world.items[static_cast<std::size_t>(item_id)];
        if (item.get_type() == item_type && item.abundance > 0) {
            candidates.push_back(item_id);
        }
    }
    if (candidates.empty()) {
        for (std::int32_t item_id = 0; static_cast<std::size_t>(item_id) < world.items.size();
             ++item_id) {
            if (world.items[static_cast<std::size_t>(item_id)].get_type() == item_type) {
                candidates.push_back(item_id);
            }
        }
    }
    if (candidates.empty()) {
        return;
    }
    std::size_t cand_cursor = 0;
    std::int32_t added = 0;
    for (std::int32_t s = kNumTroopEquipmentSlots; s < total && added < count; ++s) {
        auto& slot = troop.inventory[static_cast<std::size_t>(s)];
        if (slot.is_empty()) {
            const std::int32_t item_id = candidates[cand_cursor % candidates.size()];
            slot.clear();
            slot.item_no = item_id;
            slot.ammo = world.items[static_cast<std::size_t>(item_id)].max_ammo;
            slot.hit_points = world.items[static_cast<std::size_t>(item_id)].hit_points;
            ++cand_cursor;
            ++added;
        }
    }
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

inline void troop_equip_items(const WseWorldState& world,
                              WseTroop& troop,
                              bool remove_old = false) {
    const std::int32_t total = troop_total_inventory_slots(world, troop);
    auto target_slot_for_type = [](std::int32_t item_type) noexcept -> std::int32_t {
        switch (item_type) {
        case 12:
            return 4;  // head armor
        case 13:
            return 5;  // body armor
        case 14:
            return 6;  // foot/leg armor
        case 15:
            return 7;  // hand armor
        case 1:
            return 8;  // horse
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8:
        case 9:
        case 10:
            return 0;  // primary item/weapon/shield slot fallback
        default:
            return -1;
        }
    };
    for (std::int32_t s = kNumTroopEquipmentSlots; s < total; ++s) {
        auto& cargo = troop.inventory[static_cast<std::size_t>(s)];
        if (!world.is_valid_item(cargo.item_no)) {
            continue;
        }
        const auto& item = world.items[static_cast<std::size_t>(cargo.item_no)];
        std::int32_t eq_slot = target_slot_for_type(item.get_type());
        if (eq_slot == 0) {
            for (std::int32_t w = 0; w < 4; ++w) {
                if (troop.inventory[static_cast<std::size_t>(w)].is_empty()) {
                    eq_slot = w;
                    break;
                }
            }
        }
        if (eq_slot < 0 || eq_slot >= kNumTroopEquipmentSlots) {
            continue;
        }
        auto& eq = troop.inventory[static_cast<std::size_t>(eq_slot)];
        const std::int32_t cur_val =
            world.is_valid_item(eq.item_no)
                ? world.items[static_cast<std::size_t>(eq.item_no)].value
                : -1;
        if (eq.is_empty() || item.value > cur_val) {
            if (remove_old) {
                eq = cargo;
                cargo.clear();
            } else {
                std::swap(eq, cargo);
            }
        }
    }
}

inline void troop_add_xp(WseTroop& troop, std::int32_t amount) noexcept {
    if (amount <= 0) {
        return;
    }
    const std::int32_t clamped_add = std::min<std::int32_t>(amount, 29999);
    troop.xp += clamped_add;
    while (troop.level < 62 && troop.xp >= troop_next_level_xp(troop.level)) {
        ++troop.level;
        ++troop.attribute_points;
        ++troop.skill_points;
        troop.proficiency_points += 10;
    }
}

[[nodiscard]] inline std::int32_t party_max_companions(
    const WseWorldState& world,
    const WseParty& party) noexcept {
    std::int32_t charisma = 0;
    if (!party.stacks.empty() && world.is_valid_troop(party.stacks[0].troop_no)) {
        charisma = world.troops[static_cast<std::size_t>(party.stacks[0].troop_no)].attributes[3];
    }
    const std::int32_t leadership = party.get_skill(1, world.troops, world.skills);
    return 30 + charisma + leadership * 5;
}

[[nodiscard]] inline std::int32_t party_max_prisoners(
    const WseWorldState& world,
    const WseParty& party) noexcept {
    std::int32_t prisoner_mgmt = 0;
    if (!party.stacks.empty() && world.is_valid_troop(party.stacks[0].troop_no)) {
        const auto& leader = world.troops[static_cast<std::size_t>(party.stacks[0].troop_no)];
        const std::int32_t max_lvl = std::max<std::int32_t>(0, world.skills[2].max_level);
        prisoner_mgmt = std::min(leader.get_skill_raw(2), max_lvl);
    }
    return prisoner_mgmt * 5;
}

[[nodiscard]] inline std::int32_t party_count_troops(
    const WseParty& party,
    std::int32_t troop_no,
    bool prisoner) noexcept {
    std::int32_t total = 0;
    const std::uint32_t flag = prisoner ? 1U : 0U;
    for (const auto& s : party.stacks) {
        if (s.troop_no == troop_no && (s.flags & 1U) == flag) {
            total += s.num_troops;
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
    if (!world.is_valid_party(party_no) || count <= 0) {
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
        target->num_troops += count;
        target->num_wounded_troops += clamped_wounded;
    } else {
        party.stacks.push_back(WsePartyStack{troop_no, count, clamped_wounded, flag});
    }
    return party_count_troops(party, troop_no, prisoner) - before;
}

inline void party_add_leader(WseWorldState& world,
                             std::int32_t party_no,
                             std::int32_t troop_no,
                             std::int32_t count = 1) {
    if (!world.is_valid_party(party_no) || count <= 0) {
        return;
    }
    auto& party = world.parties[static_cast<std::size_t>(party_no)];
    for (std::size_t i = 0; i < party.stacks.size(); ++i) {
        if (party.stacks[i].troop_no == troop_no && !party.stacks[i].is_prisoner()) {
            party.stacks[i].num_troops += count;
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

inline void party_add_xp_to_stack(WseWorldState& world,
                                  std::int32_t party_no,
                                  std::int32_t stack_no,
                                  std::int32_t xp_amount) noexcept {
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
    if (troop.upgrade_troop_nos[0] < 0) {
        return;
    }
    st.xp += static_cast<float>(xp_amount);
    const float req = static_cast<float>(troop_upgrade_xp_threshold(troop));
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
    auto& st = party.stacks[static_cast<std::size_t>(stack_no)];
    st.num_upgradeable -= upg;
    st.num_troops -= upg;
    if (st.num_wounded_troops > st.num_troops) {
        st.num_wounded_troops = st.num_troops;
    }
    if (st.num_troops <= 0) {
        party.stacks.erase(party.stacks.begin() + static_cast<std::ptrdiff_t>(stack_no));
    }
    party_add_members(world, party_no, target_troop, upg, false, 0);
    return true;
}

inline void party_add_xp(WseWorldState& world,
                         std::int32_t party_no,
                         std::int32_t total_xp) noexcept {
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
        const auto& st = world.parties[static_cast<std::size_t>(party_no)].stacks[i];
        if (st.is_prisoner()) {
            break;
        }
        if (world.is_valid_troop(st.troop_no) &&
            world.troops[static_cast<std::size_t>(st.troop_no)].is_hero()) {
            const float share = (i == 0) ? (xp_per_share * 10.0f) : (xp_per_share * 3.0f);
            party_add_xp_to_stack(world, party_no, static_cast<std::int32_t>(i),
                                  static_cast<std::int32_t>(share));
        } else {
            const float share = static_cast<float>(st.num_troops) * xp_per_share;
            party_add_xp_to_stack(world, party_no, static_cast<std::int32_t>(i),
                                  static_cast<std::int32_t>(share));
        }
    }
}

inline void party_upgrade_with_xp(WseWorldState& world,
                                  std::int32_t party_no,
                                  std::int32_t total_xp,
                                  std::int32_t upgrade_path_choice) {
    if (!world.is_valid_party(party_no) || total_xp <= 0) {
        return;
    }
    party_add_xp(world, party_no, total_xp);
    const auto& party_ref = world.parties[static_cast<std::size_t>(party_no)];
    struct PendingUpgrade {
        std::int32_t src_troop;
        std::int32_t dst_troop;
        std::int32_t count;
    };
    std::vector<PendingUpgrade> upgrades;
    for (const auto& st : party_ref.stacks) {
        if (st.is_prisoner() || st.num_upgradeable <= 0 || !world.is_valid_troop(st.troop_no)) {
            continue;
        }
        const auto& tr = world.troops[static_cast<std::size_t>(st.troop_no)];
        std::int32_t path = 0;
        if (upgrade_path_choice == 1) {
            path = 0;
        } else if (upgrade_path_choice == 2) {
            path = (tr.upgrade_troop_nos[1] >= 0) ? 1 : 0;
        } else {
            path = 0;
        }
        const std::int32_t dst = tr.upgrade_troop_nos[static_cast<std::size_t>(path)];
        if (world.is_valid_troop(dst)) {
            upgrades.push_back(PendingUpgrade{st.troop_no, dst, st.num_upgradeable});
        }
    }
    for (const auto& u : upgrades) {
        auto& stacks = world.parties[static_cast<std::size_t>(party_no)].stacks;
        for (std::size_t i = 0; i < stacks.size(); ++i) {
            if (!stacks[i].is_prisoner() && stacks[i].troop_no == u.src_troop) {
                const std::int32_t p =
                    (upgrade_path_choice == 2 &&
                     world.troops[static_cast<std::size_t>(u.src_troop)].upgrade_troop_nos[1] >= 0)
                        ? 1
                        : 0;
                party_stack_upgrade(world, party_no, static_cast<std::int32_t>(i), u.count, p);
                break;
            }
        }
    }
}

inline void party_add_template(WseWorldState& world,
                               std::int32_t party_no,
                               std::int32_t template_no,
                               bool reverse_prisoner = false) {
    if (!world.is_valid_party(party_no) || !world.is_valid_party_template(template_no)) {
        return;
    }
    const auto& tpl = world.party_templates[static_cast<std::size_t>(template_no)];
    for (const auto& st : tpl.stacks) {
        if (st.troop_no < 0) {
            continue;
        }
        const std::int32_t lo = std::min(st.min_troops, st.max_troops);
        const std::int32_t hi = std::max(st.min_troops, st.max_troops);
        const std::int32_t n = (hi > lo) ? ((lo + hi) / 2) : lo;
        if (n <= 0) {
            continue;
        }
        const bool is_prisoner = ((st.flags & 1U) != 0U) != reverse_prisoner;
        party_add_members(world, party_no, st.troop_no, n, is_prisoner, 0);
    }
}

[[nodiscard]] inline bool party_can_join_party(const WseWorldState& world,
                                               std::int32_t src_party_no,
                                               std::int32_t dst_party_no,
                                               bool as_prisoners = false) noexcept {
    if (!world.is_valid_party(src_party_no) || !world.is_valid_party(dst_party_no)) {
        return false;
    }
    const auto& src = world.parties[static_cast<std::size_t>(src_party_no)];
    const auto& dst = world.parties[static_cast<std::size_t>(dst_party_no)];
    if (as_prisoners) {
        const std::int32_t incoming = src.get_num_companions() + src.get_num_prisoners();
        return dst.get_num_prisoners() + incoming <= party_max_prisoners(world, dst);
    }
    const std::int32_t free_comp = party_max_companions(world, dst) - dst.get_num_companions();
    const std::int32_t free_pris = party_max_prisoners(world, dst) - dst.get_num_prisoners();
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
    auto& src = world.parties[static_cast<std::size_t>(src_party_no)];
    src.stacks.clear();
    if (src_party_no != world.main_party_no) {
        src.active = false;
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

}  // namespace wse::game
