// Clean-room v1.1.5.5 core world/campaign module loaders reading exclusively
// through wse::platform::Vfs and wse::io::TokenReader.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/core/math.h"
#include "wse/game/module_loader_common.h"
#include "wse/game/trigger.h"
#include "wse/game/world_state.h"

namespace wse::game {

struct LoadedFactionMeta {
    std::uint32_t flags = 0;
    std::vector<std::string> ranks{};
};

struct LoadedTroopMeta {
    std::string id{};
    std::string image_mesh_name{};
};

struct LoadedItemTriggers {
    std::size_t item_index = 0;
    SimpleTriggerManager triggers{};
};

struct LoadedPartyMeta {
    std::int32_t raw_index = -1;
    std::int32_t raw_id = -1;
    std::int32_t menu_no = 0;
    wse::core::WseVec2 behavior_position{};
    float bearing = 0.0f;
};

struct LoadedSiteMeta {
    std::string mesh_name{};
    std::string body_name{};
    std::vector<std::int32_t> passages{};
    std::vector<std::int32_t> chests{};
};

struct LoadedStringEntry {
    std::string id{};
    std::string text{};
};

struct LoadedItemModifier {
    std::string id{};
    std::string name{};
    float price_multiplier = 1.0f;
    float rarity_multiplier = 1.0f;
};

namespace detail {

[[nodiscard]] inline bool ascii_iequals(std::string_view a,
                                        std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (platform::ascii_lower(a[i]) != platform::ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_native_module_dir(std::string_view dir) noexcept {
    while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
        dir.remove_suffix(1);
    }
    const std::size_t slash = dir.find_last_of("/\\");
    const std::string_view leaf =
        slash == std::string_view::npos ? dir : dir.substr(slash + 1U);
    return ascii_iequals(leaf, "Native");
}

template <class Fn>
[[nodiscard]] inline ModuleLoadError load_vfs_with_reader(platform::Vfs& vfs,
                                                          std::string_view path,
                                                          Fn&& fn) {
    std::string contents;
    ModuleLoadError err{};
    if (!read_vfs_text_checked(vfs, path, contents, err)) {
        return err;
    }
    io::TokenReader reader(contents);
    return fn(reader, path);
}

} // namespace detail

// skills.txt (0x1400cbab0 / mbSkill::readStream 0x1402135d0): zero header tokens.
[[nodiscard]] inline ModuleLoadError load_skills(
    io::TokenReader& reader,
    WseWorldState& world,
    std::string_view source = "skills.txt") {
    ModuleLoadError err{};
    std::int32_t count = 0;
    if (!read_count_token(reader, count, static_cast<std::int32_t>(kMaxSkills),
                          source, "skills", err)) {
        return err;
    }

    for (std::int32_t i = 0; i < count; ++i) {
        WseSkill skill{};
        if (!reader.read_string(skill.id) || !reader.read_string(skill.name) ||
            !read_u32_token(reader, skill.flags) ||
            !read_i32_token(reader, skill.max_level) ||
            !reader.read_string(skill.description)) {
            return make_load_error(source, "truncated skill record",
                                   static_cast<std::size_t>(i), skill.id);
        }
        underscores_to_spaces(skill.name);
        underscores_to_spaces(skill.description);
        world.skills[static_cast<std::size_t>(i)] = std::move(skill);
    }

    for (std::size_t i = static_cast<std::size_t>(count); i < kMaxSkills; ++i) {
        auto& slot = world.skills[i];
        slot.id.clear();
        slot.name.clear();
        slot.description.clear();
        slot.flags = 0x100U;
        slot.max_level = 0;
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_skills(platform::Vfs& vfs,
                                                 std::string_view path,
                                                 WseWorldState& world) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_skills(reader, world, src);
        });
}

// factions.txt (0x1400cb630 / mbFaction::readStream 0x1400c6150): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_factions(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedFactionMeta>* out_meta = nullptr,
    std::string_view source = "factions.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "factions",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.factions.clear();
    world.factions.reserve(reader.plausible_count(total, 8));
    if (out_meta != nullptr) {
        out_meta->clear();
        out_meta->reserve(reader.plausible_count(total, 8));
    }

    for (std::size_t i = 0; i < total; ++i) {
        WseFaction faction{};
        LoadedFactionMeta meta{};
        std::uint32_t color = 0;
        if (!reader.read_string(faction.id) || !reader.read_string(faction.name) ||
            !read_u32_token(reader, meta.flags) || !read_u32_token(reader, color)) {
            return make_load_error(source, "truncated faction header", i, faction.id);
        }
        underscores_to_spaces(faction.name);
        faction.color = color | 0xff000000U;

        faction.relations.resize(total, 0.0f);
        for (std::size_t r = 0; r < total; ++r) {
            if (!reader.read_f32(faction.relations[r])) {
                return make_load_error(source, "truncated faction relations", i,
                                       faction.id);
            }
        }

        std::int32_t num_ranks = 0;
        if (!read_count_token(reader, num_ranks, kMaxModuleRecords, source,
                              "faction ranks", err)) {
            err.record_index = i;
            err.record_id = faction.id;
            return err;
        }
        meta.ranks.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_ranks), 2));
        for (std::int32_t r = 0; r < num_ranks; ++r) {
            std::string rank;
            if (!reader.read_string(rank)) {
                return make_load_error(source, "truncated faction rank", i,
                                       faction.id);
            }
            meta.ranks.push_back(std::move(rank));
        }

        world.factions.push_back(std::move(faction));
        if (out_meta != nullptr) {
            out_meta->push_back(std::move(meta));
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_factions(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedFactionMeta>* out_meta = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_factions(reader, world, out_meta, src);
        });
}

// troops.txt (0x1400cbd60 / mbTroop::readStream 0x140245770):
// skips 2 strings, reads `version`, then `num_troops`.
[[nodiscard]] inline ModuleLoadError load_troops(
    io::TokenReader& reader,
    WseWorldState& world,
    std::uint32_t* prng_seed = nullptr,
    std::vector<LoadedTroopMeta>* out_meta = nullptr,
    std::string_view source = "troops.txt") {
    ModuleLoadError err{};
    std::int32_t version = 0;
    if (!read_versioned_header(reader, version, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "troops", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.troops.clear();
    world.troops.reserve(reader.plausible_count(total, 64));
    if (out_meta != nullptr) {
        out_meta->clear();
        out_meta->reserve(reader.plausible_count(total, 64));
    }

    const std::int32_t ironflesh_cap = world.skills[kSkillIronflesh].max_level;
    for (std::size_t i = 0; i < total; ++i) {
        WseTroop troop{};
        LoadedTroopMeta meta{};
        if (!reader.read_string(meta.id) || !reader.read_string(troop.name) ||
            !reader.read_string(troop.plural_name)) {
            return make_load_error(source, "truncated troop names", i, meta.id);
        }
        underscores_to_spaces(troop.name);
        underscores_to_spaces(troop.plural_name);

        if (version >= 2) {
            if (!reader.read_string(meta.image_mesh_name)) {
                return make_load_error(source, "truncated troop image mesh", i,
                                       meta.id);
            }
        } else {
            meta.image_mesh_name = "0";
        }

        std::int32_t site_and_entry = 0;
        std::int32_t unused = 0;
        if (!reader.read_u64(troop.flags) ||
            !read_i32_token(reader, site_and_entry) ||
            !read_i32_token(reader, unused) ||
            !read_i32_token(reader, troop.faction_no) ||
            !read_i32_token(reader, troop.upgrade_troop_nos[0]) ||
            !read_i32_token(reader, troop.upgrade_troop_nos[1])) {
            return make_load_error(source, "truncated troop header fields", i,
                                   meta.id);
        }
        troop.home_site_no = site_and_entry & 0xffff;
        troop.home_entry_no = (site_and_entry >> 16) & 0xff;

        for (std::size_t s = 0; s < troop.inventory.size(); ++s) {
            troop.inventory[s].clear();
        }
        for (std::size_t item_idx = 0; item_idx < 64U; ++item_idx) {
            std::int32_t item_no = -1;
            std::uint32_t item_flags = 0;
            if (!read_i32_token(reader, item_no) ||
                !read_u32_token(reader, item_flags)) {
                return make_load_error(source, "truncated troop item entry", i,
                                       meta.id);
            }
            auto& slot =
                troop.inventory[static_cast<std::size_t>(kNumTroopEquipmentSlots) +
                                item_idx];
            slot.item_no = item_no;
            if (item_no >= 0) {
                const std::uint32_t mod = (item_flags >> 24U) & 0xffU;
                slot.modifier = mod < 0x2bU ? static_cast<std::int32_t>(mod) : 0;
            }
        }

        for (std::size_t a = 0; a < kMaxAttributes; ++a) {
            if (!read_i32_token(reader, troop.attributes[a])) {
                return make_load_error(source, "truncated troop attributes", i,
                                       meta.id);
            }
        }

        std::int32_t raw_level = 1;
        if (!read_i32_token(reader, raw_level)) {
            return make_load_error(source, "truncated troop level", i, meta.id);
        }
        troop.level = std::max<std::int32_t>(1, raw_level);
        troop.xp = initial_troop_xp(troop.level, prng_seed);

        for (std::size_t p = 0; p < kMaxProficiencies; ++p) {
            std::int32_t prof_val = 0;
            if (!read_i32_token(reader, prof_val)) {
                return make_load_error(source, "truncated troop proficiencies", i,
                                       meta.id);
            }
            troop.proficiencies[p] = static_cast<float>(prof_val);
        }

        for (std::size_t sk = 0; sk < troop.skills.size(); ++sk) {
            if (!read_u32_token(reader, troop.skills[sk])) {
                return make_load_error(source, "truncated troop skills", i,
                                       meta.id);
            }
        }

        // Skill 36 (kSkillIronflesh) lives at skills[4] nibble 4 (bits 16..19).
        const auto raw_ironflesh =
            static_cast<std::int32_t>((troop.skills[4] >> 16U) & 0xfU);
        const std::int32_t clamped_ironflesh =
            std::min(raw_ironflesh, std::max<std::int32_t>(0, ironflesh_cap));
        troop.hit_points =
            static_cast<float>(troop.attributes[0] + 35 + clamped_ironflesh * 2);

        for (std::size_t fk = 0; fk < 2U; ++fk) {
            for (std::size_t w = 0; w < 4U; ++w) {
                if (!reader.read_u64(troop.face_keys[fk][w])) {
                    return make_load_error(source, "truncated troop face keys", i,
                                           meta.id);
                }
            }
        }

        troop.skin_no = static_cast<std::int32_t>(troop.flags & 0xfULL);
        world.troops.push_back(std::move(troop));
        if (out_meta != nullptr) {
            out_meta->push_back(std::move(meta));
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_troops(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::uint32_t* prng_seed = nullptr,
    std::vector<LoadedTroopMeta>* out_meta = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_troops(reader, world, prng_seed, out_meta, src);
        });
}

// item_kinds1.txt (0x140097350 / mbItemKind::readStream 0x14010f8f0):
// skips 2 strings, reads `version`, then `num_items`.
[[nodiscard]] inline ModuleLoadError load_item_kinds(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedItemTriggers>* out_triggers = nullptr,
    std::string_view source = "item_kinds1.txt") {
    ModuleLoadError err{};
    std::int32_t version = 0;
    if (!read_versioned_header(reader, version, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "item kinds",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.items.clear();
    world.items.reserve(reader.plausible_count(total, 32));
    if (out_triggers != nullptr) {
        out_triggers->clear();
    }

    for (std::size_t i = 0; i < total; ++i) {
        WseItemKind item{};
        if (!reader.read_string(item.id) || !reader.read_string(item.name) ||
            !reader.read_string(item.plural_name)) {
            return make_load_error(source, "truncated item names", i, item.id);
        }
        underscores_to_spaces(item.name);
        underscores_to_spaces(item.plural_name);

        std::int32_t num_variations = 0;
        if (!read_count_token(reader, num_variations, 16, source,
                              "item variations", err)) {
            err.record_index = i;
            err.record_id = item.id;
            return err;
        }
        item.variations.clear();
        item.variations.reserve(static_cast<std::size_t>(num_variations));
        for (std::int32_t v = 0; v < num_variations; ++v) {
            WseItemKindVariation var{};
            if (!reader.read_string(var.mesh_name) ||
                !reader.read_u64(var.flags)) {
                return make_load_error(source, "truncated item variation", i,
                                       item.id);
            }
            item.variations.push_back(std::move(var));
        }

        if (!reader.read_u64(item.properties) ||
            !reader.read_u64(item.capabilities) ||
            !read_i32_token(reader, item.value) ||
            !reader.read_u64(item.modifiers) || !reader.read_f32(item.weight) ||
            !read_i32_token(reader, item.abundance) ||
            !read_i32_token(reader, item.head_armor) ||
            !read_i32_token(reader, item.body_armor) ||
            !read_i32_token(reader, item.leg_armor) ||
            !read_i32_token(reader, item.difficulty) ||
            !read_i32_token(reader, item.hit_points) ||
            !read_i32_token(reader, item.speed_rating) ||
            !read_i32_token(reader, item.missile_speed) ||
            !read_i32_token(reader, item.weapon_length) ||
            !read_i32_token(reader, item.max_ammo) ||
            !read_i32_token(reader, item.thrust_damage) ||
            !read_i32_token(reader, item.swing_damage)) {
            return make_load_error(source, "truncated item attributes", i, item.id);
        }

        if (version > 2) {
            std::int32_t num_factions = 0;
            if (!read_count_token(reader, num_factions, kMaxModuleRecords, source,
                                  "item factions", err)) {
                err.record_index = i;
                err.record_id = item.id;
                return err;
            }
            item.factions.resize(static_cast<std::size_t>(num_factions));
            for (std::int32_t f = 0; f < num_factions; ++f) {
                if (!read_i32_token(reader,
                                    item.factions[static_cast<std::size_t>(f)])) {
                    return make_load_error(source, "truncated item faction", i,
                                           item.id);
                }
            }
        }

        if (version > 1) {
            constexpr std::array<std::int64_t, 3> kZeroTimers{};
            SimpleTriggerManager item_triggers{};
            const std::string ctx_name = "Item " + item.id;
            const vm::ReadError trig_err = read_simple_trigger_manager(
                reader, item_triggers, ctx_name, kZeroTimers);
            if (trig_err != vm::ReadError::none) {
                return make_read_error(source, trig_err, i, item.id);
            }
            if (out_triggers != nullptr && !item_triggers.triggers.empty()) {
                out_triggers->push_back(
                    LoadedItemTriggers{i, std::move(item_triggers)});
            }
        }

        world.items.push_back(std::move(item));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_item_kinds(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedItemTriggers>* out_triggers = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_item_kinds(reader, world, out_triggers, src);
        });
}

// party_templates.txt (0x140098130 / mbPartyTemplate::readStream 0x1401c3bc0): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_party_templates(
    io::TokenReader& reader,
    WseWorldState& world,
    std::string_view source = "party_templates.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "party templates", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.party_templates.clear();
    world.party_templates.reserve(reader.plausible_count(total, 16));

    for (std::size_t i = 0; i < total; ++i) {
        WsePartyTemplate tmpl{};
        if (!reader.read_string(tmpl.id) || !reader.read_string(tmpl.name) ||
            !reader.read_u64(tmpl.flags) || !read_i32_token(reader, tmpl.menu_no) ||
            !read_i32_token(reader, tmpl.faction_no) ||
            !read_u32_token(reader, tmpl.personality)) {
            return make_load_error(source, "truncated party template", i, tmpl.id);
        }
        underscores_to_spaces(tmpl.name);
        tmpl.icon_no = static_cast<std::int32_t>(tmpl.flags & 0xffULL);

        for (std::size_t s = 0; s < tmpl.stacks.size(); ++s) {
            std::int32_t troop_no = -1;
            if (!read_i32_token(reader, troop_no)) {
                return make_load_error(source, "truncated party template stack", i,
                                       tmpl.id);
            }
            if (troop_no > -1) {
                auto& st = tmpl.stacks[s];
                st.troop_no = troop_no;
                if (!read_i32_token(reader, st.min_troops) ||
                    !read_i32_token(reader, st.max_troops) ||
                    !read_u32_token(reader, st.flags)) {
                    return make_load_error(source,
                                           "truncated party template stack data",
                                           i, tmpl.id);
                }
            } else {
                tmpl.stacks[s] = {};
            }
        }
        world.party_templates.push_back(std::move(tmpl));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_party_templates(platform::Vfs& vfs,
                                                          std::string_view path,
                                                          WseWorldState& world) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_party_templates(reader, world, src);
        });
}

// parties.txt (0x1400cd7e0 / rglHashVector<mbParty> 0x1400e7ce0 / mbParty::readStream 0x1401ba760).
[[nodiscard]] inline ModuleLoadError load_parties(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedPartyMeta>* out_meta = nullptr,
    std::string_view source = "parties.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t num_slots = 0;
    std::int32_t num_created = 0;
    // Party IDs index the dense `parties` vector up to `next_party_key`, so both counts are
    // bounded by `kMaxPartyKeys` rather than the generic record limit.
    if (!read_count_token(reader, num_slots, kMaxPartyKeys, source,
                          "party slots", err) ||
        !read_i32_token(reader, num_created) || num_created < 0 ||
        num_created > kMaxPartyKeys) {
        return err.ok ? make_load_error(source, "invalid party created count") : err;
    }

    const auto total_slots = static_cast<std::size_t>(num_slots);
    if (reader.plausible_count(total_slots, 1) < total_slots) {
        return make_load_error(source, "truncated parties table");
    }

    world.parties.clear();
    world.parties.resize(total_slots);
    world.next_party_key = std::max(num_slots, num_created);
    if (out_meta != nullptr) {
        out_meta->assign(total_slots, LoadedPartyMeta{});
    }

    const bool filter_troops = !world.troops.empty();
    for (std::size_t s = 0; s < total_slots; ++s) {
        std::int32_t is_active = 0;
        if (!read_i32_token(reader, is_active)) {
            return make_load_error(source, "truncated party slot active flag", s);
        }
        if (is_active == 0) {
            world.parties[s].active = false;
            continue;
        }

        LoadedPartyMeta meta{};
        WseParty party{};
        party.active = true;
        if (!read_i32_token(reader, meta.raw_index) ||
            !read_i32_token(reader, meta.raw_id) ||
            !reader.read_string(party.id) || !reader.read_string(party.name) ||
            !reader.read_u64(party.flags) ||
            !read_i32_token(reader, meta.menu_no) ||
            !read_i32_token(reader, party.template_no) ||
            !read_i32_token(reader, party.faction_no) ||
            !read_u32_token(reader, party.personality) ||
            !read_i32_token(reader, party.default_behavior) ||
            !read_i32_token(reader, party.behavior) ||
            !read_i32_token(reader, party.default_behavior_object) ||
            !read_i32_token(reader, party.behavior_object) ||
            !reader.read_f32(party.default_behavior_position.x) ||
            !reader.read_f32(party.default_behavior_position.y) ||
            !reader.read_f32(meta.behavior_position.x) ||
            !reader.read_f32(meta.behavior_position.y)) {
            return make_load_error(source, "truncated party header", s, party.id);
        }
        underscores_to_spaces(party.name);
        party.icon_no = static_cast<std::int32_t>(party.flags & 0xffULL);

        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        std::int32_t num_stacks = 0;
        if (!reader.read_f32(px) || !reader.read_f32(py) || !reader.read_f32(pz) ||
            !read_count_token(reader, num_stacks, kMaxModuleRecords, source,
                              "party stacks", err)) {
            return err.ok ? make_load_error(source, "truncated party position/stacks",
                                            s, party.id)
                          : err;
        }

        const auto stack_count = static_cast<std::size_t>(num_stacks);
        party.stacks.reserve(reader.plausible_count(stack_count, 8));
        for (std::size_t k = 0; k < stack_count; ++k) {
            WsePartyStack st{};
            if (!read_i32_token(reader, st.troop_no) ||
                !read_i32_token(reader, st.num_troops) ||
                !read_i32_token(reader, st.num_wounded_troops) ||
                !read_u32_token(reader, st.flags)) {
                return make_load_error(source, "truncated party stack", s, party.id);
            }
            if (!filter_troops ||
                (st.troop_no >= 0 &&
                 static_cast<std::size_t>(st.troop_no) < world.troops.size())) {
                party.stacks.push_back(st);
            }
        }

        if (!reader.read_f32(meta.bearing)) {
            return make_load_error(source, "truncated party bearing", s, party.id);
        }
        party.position = wse::core::WseTransform::identity();
        party.position.o = {px, py, pz, 1.0f};
        if (meta.bearing != 0.0f) {
            party.position.rotate_z(meta.bearing);
        }

        const std::uint32_t bandit_nibble =
            static_cast<std::uint32_t>((party.flags >> 24U) & 0xfULL);
        party.bandit_attraction = static_cast<std::int32_t>(
            std::lround(static_cast<float>(bandit_nibble) * (100.0f / 30.0f)));

        world.parties[s] = std::move(party);
        if (out_meta != nullptr) {
            (*out_meta)[s] = meta;
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_parties(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedPartyMeta>* out_meta = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_parties(reader, world, out_meta, src);
        });
}

// scenes.txt (0x1400cc1f0 / mbSite::readStream 0x140210950): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_sites(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedSiteMeta>* out_meta = nullptr,
    std::string_view source = "scenes.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "sites", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.sites.clear();
    world.sites.reserve(reader.plausible_count(total, 24));
    if (out_meta != nullptr) {
        out_meta->clear();
        out_meta->reserve(reader.plausible_count(total, 24));
    }

    for (std::size_t i = 0; i < total; ++i) {
        WseSite site{};
        LoadedSiteMeta meta{};
        wse::core::WseVec2 min_bound{};
        wse::core::WseVec2 max_bound{};
        std::string terrain_code;
        std::string outer_terrain;
        if (!reader.read_string(site.id) || !reader.read_string(site.name) ||
            !read_u32_token(reader, site.flags) ||
            !reader.read_string(meta.mesh_name) ||
            !reader.read_string(meta.body_name) || !reader.read_f32(min_bound.x) ||
            !reader.read_f32(min_bound.y) || !reader.read_f32(max_bound.x) ||
            !reader.read_f32(max_bound.y) ||
            !reader.read_f32(site.water_level) || !reader.read_string(terrain_code)) {
            return make_load_error(source, "truncated site header", i, site.id);
        }

        std::int32_t num_passages = 0;
        if (!read_count_token(reader, num_passages, kMaxModuleRecords, source,
                              "site passages", err)) {
            err.record_index = i;
            err.record_id = site.id;
            return err;
        }
        meta.passages.resize(static_cast<std::size_t>(num_passages));
        for (std::int32_t p = 0; p < num_passages; ++p) {
            if (!read_i32_token(reader, meta.passages[static_cast<std::size_t>(p)])) {
                return make_load_error(source, "truncated site passage", i, site.id);
            }
        }

        std::int32_t num_chests = 0;
        if (!read_count_token(reader, num_chests, kMaxModuleRecords, source,
                              "site chests", err)) {
            err.record_index = i;
            err.record_id = site.id;
            return err;
        }
        meta.chests.resize(static_cast<std::size_t>(num_chests));
        for (std::int32_t c = 0; c < num_chests; ++c) {
            if (!read_i32_token(reader, meta.chests[static_cast<std::size_t>(c)])) {
                return make_load_error(source, "truncated site chest", i, site.id);
            }
        }

        if (!reader.read_string(outer_terrain)) {
            return make_load_error(source, "truncated site outer terrain", i,
                                   site.id);
        }

        auto& m6_site = world.m6.site(static_cast<std::int32_t>(i));
        m6_site.flags = static_cast<std::int32_t>(site.flags);
        m6_site.water_level = site.water_level;
        m6_site.min_bound = min_bound;
        m6_site.max_bound = max_bound;
        m6_site.terrain_code = std::move(terrain_code);
        m6_site.outer_terrain = std::move(outer_terrain);

        world.sites.push_back(std::move(site));
        if (out_meta != nullptr) {
            out_meta->push_back(std::move(meta));
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_sites(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedSiteMeta>* out_meta = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_sites(reader, world, out_meta, src);
        });
}

// quests.txt (0x1400cc670 / mbQuest::readStream 0x1401c4690): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_quests(
    io::TokenReader& reader,
    WseWorldState& world,
    std::string_view source = "quests.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "quests", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.quests.clear();
    world.quests.reserve(reader.plausible_count(total, 8));

    for (std::size_t i = 0; i < total; ++i) {
        WseQuest q{};
        if (!reader.read_string(q.id) || !reader.read_string(q.title) ||
            !read_u32_token(reader, q.template_flags) ||
            !reader.read_string(q.text)) {
            return make_load_error(source, "truncated quest record", i, q.id);
        }
        underscores_to_spaces(q.title);
        underscores_to_spaces(q.text);
        world.quests.push_back(std::move(q));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_quests(platform::Vfs& vfs,
                                                 std::string_view path,
                                                 WseWorldState& world) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_quests(reader, world, src);
        });
}

// info_pages.txt (0x1400ccae0): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_info_pages(
    io::TokenReader& reader,
    WseWorldState& world,
    std::string_view source = "info_pages.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "info pages",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.info_pages.clear();
    world.info_pages.reserve(reader.plausible_count(total, 6));

    for (std::size_t i = 0; i < total; ++i) {
        WseInfoPage page{};
        if (!reader.read_string(page.id) || !reader.read_string(page.title) ||
            !reader.read_string(page.text)) {
            return make_load_error(source, "truncated info page record", i,
                                   page.id);
        }
        underscores_to_spaces(page.title);
        underscores_to_spaces(page.text);
        world.info_pages.push_back(std::move(page));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_info_pages(platform::Vfs& vfs,
                                                     std::string_view path,
                                                     WseWorldState& world) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_info_pages(reader, world, src);
        });
}

// strings.txt (0x140096ea0): 3 header tokens; quick_strings.txt (0x140097240): 0 header tokens.
[[nodiscard]] inline ModuleLoadError load_string_bank(
    io::TokenReader& reader,
    std::size_t header_skip_tokens,
    std::vector<LoadedStringEntry>& out,
    std::string_view source) {
    ModuleLoadError err{};
    if (header_skip_tokens > 0 &&
        !skip_header_tokens(reader, header_skip_tokens, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "strings",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    out.clear();
    out.reserve(reader.plausible_count(total, 4));
    for (std::size_t i = 0; i < total; ++i) {
        LoadedStringEntry entry{};
        if (!reader.read_string(entry.id) || !reader.read_string(entry.text)) {
            return make_load_error(source, "truncated string entry", i, entry.id);
        }
        underscores_to_spaces(entry.text);
        out.push_back(std::move(entry));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_strings(
    io::TokenReader& reader,
    std::vector<LoadedStringEntry>& out,
    std::string_view source = "strings.txt") {
    return load_string_bank(reader, 3, out, source);
}

[[nodiscard]] inline ModuleLoadError load_strings(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedStringEntry>& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_strings(reader, out, src);
        });
}

[[nodiscard]] inline ModuleLoadError load_quick_strings(
    io::TokenReader& reader,
    std::vector<LoadedStringEntry>& out,
    std::string_view source = "quick_strings.txt") {
    return load_string_bank(reader, 0, out, source);
}

[[nodiscard]] inline ModuleLoadError load_quick_strings(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedStringEntry>& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_quick_strings(reader, out, src);
        });
}

// game_variables.txt (0x14008c650): Native fallback followed by module override.
inline void load_game_variables(platform::Vfs& vfs,
                                std::string_view module_dir,
                                std::string_view native_dir,
                                WseWorldState& world) {
    world.game_variables.clear();
    std::string text;
    if (!native_dir.empty() && !detail::is_native_module_dir(module_dir)) {
        const std::string native_path =
            vfs_join_path(native_dir, "game_variables.txt");
        if (vfs.read_text(native_path, text)) {
            parse_sectionless_ini(text, world.game_variables);
        }
    }
    const std::string mod_path =
        vfs_join_path(module_dir, "game_variables.txt");
    if (vfs.read_text(mod_path, text)) {
        parse_sectionless_ini(text, world.game_variables);
    }
}

[[nodiscard]] inline ModuleLoadError load_module_ini(platform::Vfs& vfs,
                                                     std::string_view module_dir,
                                                     ModuleIniData& out) {
    const std::string path = vfs_join_path(module_dir, "module.ini");
    std::string text;
    ModuleLoadError err{};
    if (!read_vfs_text_checked(vfs, path, text, err)) {
        return err;
    }
    parse_module_ini(text, out);
    return {};
}

// INI sections (0x1402a6ff0) into world.module_settings; returns false if missing.
inline bool load_module_settings(platform::Vfs& vfs,
                                 std::string_view ini_path,
                                 WseWorldState& world) {
    std::string text;
    if (!vfs.read_text(ini_path, text)) {
        return false;
    }
    parse_ini_sections(text, world.module_settings);
    return true;
}

// Data/item_modifiers.txt (0x140093f80): fixed 43 entries (no count token).
[[nodiscard]] inline ModuleLoadError load_item_modifiers(
    io::TokenReader& reader,
    std::vector<LoadedItemModifier>& out,
    std::string_view source = "Data/item_modifiers.txt") {
    constexpr std::size_t kNumItemModifiers = 43U;
    out.clear();
    out.resize(kNumItemModifiers);
    for (std::size_t i = 0; i < kNumItemModifiers; ++i) {
        auto& mod = out[i];
        if (!reader.read_string(mod.id) || !reader.read_string(mod.name) ||
            !reader.read_f32(mod.price_multiplier) ||
            !reader.read_f32(mod.rarity_multiplier)) {
            return make_load_error(source, "truncated item modifier", i, mod.id);
        }
        underscores_to_spaces(mod.name);
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_item_modifiers(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedItemModifier>& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_item_modifiers(reader, out, src);
        });
}

} // namespace wse::game
