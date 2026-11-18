// Clean-room v1.1.5.5 module mission, menu, scene-prop, presentation, tableau,
// map-icon, and conversation loaders reading through Vfs and TokenReader.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/game/module_loader_common.h"
#include "wse/game/module_world_loaders.h"
#include "wse/game/trigger.h"
#include "wse/game/world_state.h"
#include "wse/vm/statement.h"

namespace wse::game {

struct LoadedMapIcon {
    std::string id{};
    std::uint32_t flags = 0;
    std::string mesh_name{};
    float scale = 1.0f;
    std::int32_t sound_no = -1;
    std::array<float, 3> offset{};
    SimpleTriggerManager triggers{};
};

struct LoadedSceneProp {
    std::string id{};
    std::uint64_t flags = 0;
    std::int32_t hit_points = 0;
    std::string mesh_name{};
    std::string body_name{};
    SimpleTriggerManager triggers{};
};

struct LoadedMenuOption {
    std::string id{};
    vm::StatementBlock condition{};
    std::string text{};
    vm::StatementBlock consequence{};
    std::string door_name{};
};

struct LoadedMenu {
    std::string id{};
    std::uint64_t flags = 0;
    std::string text{};
    std::string mesh_name{};
    vm::StatementBlock operations{};
    std::vector<LoadedMenuOption> options{};
};

struct LoadedPresentation {
    std::string id{};
    std::uint32_t flags = 0;
    std::int32_t mesh_no = -1;
    SimpleTriggerManager triggers{};
};

struct LoadedMissionTemplateGroup {
    std::int32_t entry_point = 0;
    std::uint32_t flags = 0;
    std::uint32_t alter_flags = 0;
    std::uint32_t ai_flags = 0;
    std::int32_t num_troops = 0;
    std::vector<std::int32_t> override_items{};
};

struct LoadedMissionTemplateRecord {
    std::string id{};
    std::string name{};
    std::uint32_t flags = 0;
    std::int32_t unused = 0;
    std::string description{};
    std::vector<LoadedMissionTemplateGroup> groups{};
    TriggerManager triggers{};
};

struct LoadedTableauMaterial {
    std::string id{};
    std::uint32_t flags = 0;
    std::string material_name{};
    std::int32_t width = 0;
    std::int32_t height = 0;
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    vm::StatementBlock operations{};
};

struct LoadedConversationSentence {
    std::string id{};
    std::uint32_t flags = 0;
    std::int32_t input_token = 0;
    vm::StatementBlock conditions{};
    std::string text{};
    std::int32_t output_token = 0;
    vm::StatementBlock consequences{};
    std::string voiceover{};
};

// map_icons.txt (0x140096940 / mbMapIcon::readStream 0x140116d80): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_map_icons(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedMapIcon>* out_icons = nullptr,
    std::string_view source = "map_icons.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "map icons",
                          err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_icons != nullptr) {
        out_icons->clear();
        out_icons->reserve(reader.plausible_count(total, 16));
    }
    constexpr std::array<std::int64_t, 3> kZeroTimers{};
    for (std::size_t i = 0; i < total; ++i) {
        LoadedMapIcon icon{};
        if (!reader.read_string(icon.id) || !read_u32_token(reader, icon.flags) ||
            !reader.read_string(icon.mesh_name) ||
            !reader.read_f32(icon.scale) || !read_i32_token(reader, icon.sound_no) ||
            !reader.read_f32(icon.offset[0]) || !reader.read_f32(icon.offset[1]) ||
            !reader.read_f32(icon.offset[2])) {
            return make_load_error(source, "truncated map icon", i, icon.id);
        }
        const std::string ctx = "Map Icon " + icon.id;
        const vm::ReadError trig_err = read_simple_trigger_manager(
            reader, icon.triggers, ctx, kZeroTimers);
        if (trig_err != vm::ReadError::none) {
            return make_read_error(source, trig_err, i, icon.id);
        }
        if (out_icons != nullptr) {
            out_icons->push_back(std::move(icon));
        }
    }
    world.misc.num_map_icons = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_map_icons(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedMapIcon>* out_icons = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_map_icons(reader, world, out_icons, src);
        });
}

// scene_props.txt (0x1400954c0 / mbSceneProp::readStream 0x14020f320): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_scene_props(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedSceneProp>* out_props = nullptr,
    std::string_view source = "scene_props.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "scene props", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_props != nullptr) {
        out_props->clear();
        out_props->reserve(reader.plausible_count(total, 12));
    }
    constexpr std::array<std::int64_t, 3> kZeroTimers{};
    for (std::size_t i = 0; i < total; ++i) {
        LoadedSceneProp prop{};
        if (!reader.read_string(prop.id) || !reader.read_u64(prop.flags) ||
            !read_i32_token(reader, prop.hit_points) ||
            !reader.read_string(prop.mesh_name) ||
            !reader.read_string(prop.body_name)) {
            return make_load_error(source, "truncated scene prop", i, prop.id);
        }
        const std::string ctx = "Scene Prop " + prop.id;
        prop.triggers.defaultTimerNo = 2;
        const vm::ReadError trig_err = read_simple_trigger_manager(
            reader, prop.triggers, ctx, kZeroTimers);
        if (trig_err != vm::ReadError::none) {
            return make_read_error(source, trig_err, i, prop.id);
        }
        if (out_props != nullptr) {
            out_props->push_back(std::move(prop));
        }
    }
    world.misc.num_scene_props = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_scene_props(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedSceneProp>* out_props = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_scene_props(reader, world, out_props, src);
        });
}

// menus.txt (0x1400959d0 / mbMenu::readStream 0x140120090): 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_menus(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedMenu>* out_menus = nullptr,
    std::string_view source = "menus.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source, "menus", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_menus != nullptr) {
        out_menus->clear();
        out_menus->reserve(reader.plausible_count(total, 16));
    }
    for (std::size_t i = 0; i < total; ++i) {
        LoadedMenu menu{};
        if (!reader.read_string(menu.id) || !reader.read_u64(menu.flags) ||
            !reader.read_string(menu.text) ||
            !reader.read_string(menu.mesh_name)) {
            return make_load_error(source, "truncated menu header", i, menu.id);
        }
        underscores_to_spaces(menu.text);

        const vm::ReadError op_err =
            vm::read_statement_block(reader, menu.operations);
        if (op_err != vm::ReadError::none) {
            return make_read_error(source, op_err, i, menu.id);
        }

        std::int32_t num_options = 0;
        if (!read_count_token(reader, num_options, kMaxModuleRecords, source,
                              "menu options", err)) {
            err.record_index = i;
            err.record_id = menu.id;
            return err;
        }
        menu.options.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_options), 8));
        for (std::int32_t opt_idx = 0; opt_idx < num_options; ++opt_idx) {
            LoadedMenuOption opt{};
            if (!reader.read_string(opt.id)) {
                return make_load_error(source, "truncated menu option id", i,
                                       menu.id);
            }
            const vm::ReadError cond_err =
                vm::read_statement_block(reader, opt.condition);
            if (cond_err != vm::ReadError::none) {
                return make_read_error(source, cond_err, i, menu.id);
            }
            if (!reader.read_string(opt.text)) {
                return make_load_error(source, "truncated menu option text", i,
                                       menu.id);
            }
            underscores_to_spaces(opt.text);
            const vm::ReadError cons_err =
                vm::read_statement_block(reader, opt.consequence);
            if (cons_err != vm::ReadError::none) {
                return make_read_error(source, cons_err, i, menu.id);
            }
            if (!reader.read_string(opt.door_name)) {
                return make_load_error(source, "truncated menu option door", i,
                                       menu.id);
            }
            underscores_to_spaces(opt.door_name);
            menu.options.push_back(std::move(opt));
        }

        if (out_menus != nullptr) {
            out_menus->push_back(std::move(menu));
        }
    }
    world.misc.num_menus = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_menus(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedMenu>* out_menus = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_menus(reader, world, out_menus, src);
        });
}

// presentations.txt (0x140096720 / mbPresentationManager::readStream 0x1401c3e50 / mbPresentation::readStream 0x1401c3d00):
// skips 2 strings + discarded version i32 = 3 header tokens.
[[nodiscard]] inline ModuleLoadError load_presentations(
    io::TokenReader& reader,
    std::vector<LoadedPresentation>& out,
    std::string_view source = "presentations.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "presentations", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    out.clear();
    out.reserve(reader.plausible_count(total, 8));
    constexpr std::array<std::int64_t, 3> kZeroTimers{};
    for (std::size_t i = 0; i < total; ++i) {
        LoadedPresentation prsnt{};
        if (!reader.read_string(prsnt.id) ||
            !read_u32_token(reader, prsnt.flags) ||
            !read_i32_token(reader, prsnt.mesh_no)) {
            return make_load_error(source, "truncated presentation", i, prsnt.id);
        }
        const std::string ctx = "Presentation " + prsnt.id;
        const vm::ReadError trig_err = read_simple_trigger_manager(
            reader, prsnt.triggers, ctx, kZeroTimers);
        if (trig_err != vm::ReadError::none) {
            return make_read_error(source, trig_err, i, prsnt.id);
        }
        out.push_back(std::move(prsnt));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_presentations(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedPresentation>& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_presentations(reader, out, src);
        });
}

// mission_templates.txt (0x140097c20 / mbMissionTemplate::readStream 0x140179fb0):
// 3 header tokens; underscores are converted on description, not name.
[[nodiscard]] inline ModuleLoadError load_mission_templates(
    io::TokenReader& reader,
    WseWorldState& world,
    std::span<const std::int64_t> timers_total = {},
    std::vector<LoadedMissionTemplateRecord>* out_records = nullptr,
    std::string_view source = "mission_templates.txt") {
    ModuleLoadError err{};
    if (!skip_header_tokens(reader, 3, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "mission templates", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    world.mission_templates.clear();
    world.mission_templates.reserve(reader.plausible_count(total, 16));
    if (out_records != nullptr) {
        out_records->clear();
        out_records->reserve(reader.plausible_count(total, 16));
    }

    constexpr std::array<std::int64_t, 3> kDefaultTimers{};
    const std::span<const std::int64_t> timers =
        timers_total.empty() ? std::span<const std::int64_t>(kDefaultTimers)
                             : timers_total;

    for (std::size_t i = 0; i < total; ++i) {
        LoadedMissionTemplateRecord rec{};
        std::int32_t num_groups = 0;
        if (!reader.read_string(rec.id) || !reader.read_string(rec.name) ||
            !read_u32_token(reader, rec.flags) ||
            !read_i32_token(reader, rec.unused) ||
            !reader.read_string(rec.description) ||
            !read_count_token(reader, num_groups, kMaxModuleRecords, source,
                              "mission template groups", err)) {
            return err.ok ? make_load_error(source, "truncated mission template",
                                            i, rec.id)
                          : err;
        }
        underscores_to_spaces(rec.description);

        WseMissionTemplate world_mt{};
        world_mt.id = rec.id;
        world_mt.name = rec.name;

        rec.groups.reserve(
            reader.plausible_count(static_cast<std::size_t>(num_groups), 12));
        for (std::int32_t g = 0; g < num_groups; ++g) {
            LoadedMissionTemplateGroup grp{};
            std::int32_t num_overrides = 0;
            if (!read_i32_token(reader, grp.entry_point) ||
                !read_u32_token(reader, grp.flags) ||
                !read_u32_token(reader, grp.alter_flags) ||
                !read_u32_token(reader, grp.ai_flags) ||
                !read_i32_token(reader, grp.num_troops) ||
                !read_count_token(reader, num_overrides, kMaxModuleRecords, source,
                                  "entry override items", err)) {
                return err.ok ? make_load_error(
                                    source, "truncated mission template group", i,
                                    rec.id)
                              : err;
            }
            grp.override_items.resize(static_cast<std::size_t>(num_overrides));
            for (std::int32_t ov = 0; ov < num_overrides; ++ov) {
                if (!read_i32_token(
                        reader,
                        grp.override_items[static_cast<std::size_t>(ov)])) {
                    return make_load_error(
                        source, "truncated entry override item", i, rec.id);
                }
            }
            if (grp.entry_point >= 0 &&
                static_cast<std::size_t>(grp.entry_point) < kMaxEntryPoints) {
                auto& entry = world_mt.entry_overrides[static_cast<std::size_t>(
                    grp.entry_point)];
                entry.flags = grp.flags;
                entry.override_items = grp.override_items;
            }
            rec.groups.push_back(std::move(grp));
        }

        const std::string ctx = "Mission Template " + rec.id;
        const vm::ReadError trig_err =
            read_trigger_manager(reader, rec.triggers, ctx, timers);
        if (trig_err != vm::ReadError::none) {
            return make_read_error(source, trig_err, i, rec.id);
        }

        world.mission_templates.push_back(std::move(world_mt));
        if (out_records != nullptr) {
            out_records->push_back(std::move(rec));
        }
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_mission_templates(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::span<const std::int64_t> timers_total = {},
    std::vector<LoadedMissionTemplateRecord>* out_records = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_mission_templates(reader, world, timers_total,
                                          out_records, src);
        });
}

// tableau_materials.txt (0x1400965b0 / mbTableauMaterial::readStream 0x14021a490):
// zero header tokens; min/max coordinates are floats (readFloat).
[[nodiscard]] inline ModuleLoadError load_tableau_materials(
    io::TokenReader& reader,
    WseWorldState& world,
    std::vector<LoadedTableauMaterial>* out_mats = nullptr,
    std::string_view source = "tableau_materials.txt") {
    ModuleLoadError err{};
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "tableau materials", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    if (out_mats != nullptr) {
        out_mats->clear();
        out_mats->reserve(reader.plausible_count(total, 16));
    }
    for (std::size_t i = 0; i < total; ++i) {
        LoadedTableauMaterial mat{};
        if (!reader.read_string(mat.id) || !read_u32_token(reader, mat.flags) ||
            !reader.read_string(mat.material_name) ||
            !read_i32_token(reader, mat.width) ||
            !read_i32_token(reader, mat.height) || !reader.read_f32(mat.min_x) ||
            !reader.read_f32(mat.min_y) || !reader.read_f32(mat.max_x) ||
            !reader.read_f32(mat.max_y)) {
            return make_load_error(source, "truncated tableau material", i,
                                   mat.id);
        }
        const vm::ReadError op_err =
            vm::read_statement_block(reader, mat.operations);
        if (op_err != vm::ReadError::none) {
            return make_read_error(source, op_err, i, mat.id);
        }
        if (out_mats != nullptr) {
            out_mats->push_back(std::move(mat));
        }
    }
    world.misc.num_tableau_materials = count;
    return {};
}

[[nodiscard]] inline ModuleLoadError load_tableau_materials(
    platform::Vfs& vfs,
    std::string_view path,
    WseWorldState& world,
    std::vector<LoadedTableauMaterial>* out_mats = nullptr) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_tableau_materials(reader, world, out_mats, src);
        });
}

// conversation.txt (0x140097870 / mbConversationManager::readStream 0x140084730):
// skips 2 strings + `version`, reads `num_sentences`; `version > 1` adds `voiceover`.
[[nodiscard]] inline ModuleLoadError load_conversations(
    io::TokenReader& reader,
    std::vector<LoadedConversationSentence>& out,
    std::string_view source = "conversation.txt") {
    ModuleLoadError err{};
    std::int32_t version = 0;
    if (!read_versioned_header(reader, version, source, err)) {
        return err;
    }
    std::int32_t count = 0;
    if (!read_count_token(reader, count, kMaxModuleRecords, source,
                          "conversation sentences", err)) {
        return err;
    }

    const auto total = static_cast<std::size_t>(count);
    out.clear();
    out.reserve(reader.plausible_count(total, 16));
    for (std::size_t i = 0; i < total; ++i) {
        LoadedConversationSentence s{};
        if (!reader.read_string(s.id) || !read_u32_token(reader, s.flags) ||
            !read_i32_token(reader, s.input_token)) {
            return make_load_error(source, "truncated conversation sentence", i,
                                   s.id);
        }
        const vm::ReadError cond_err =
            vm::read_statement_block(reader, s.conditions);
        if (cond_err != vm::ReadError::none) {
            return make_read_error(source, cond_err, i, s.id);
        }
        if (!reader.read_string(s.text) ||
            !read_i32_token(reader, s.output_token)) {
            return make_load_error(source, "truncated conversation text/output",
                                   i, s.id);
        }
        underscores_to_spaces(s.text);
        const vm::ReadError cons_err =
            vm::read_statement_block(reader, s.consequences);
        if (cons_err != vm::ReadError::none) {
            return make_read_error(source, cons_err, i, s.id);
        }
        if (version > 1 && !reader.read_string(s.voiceover)) {
            return make_load_error(source, "truncated conversation voiceover", i,
                                   s.id);
        }
        out.push_back(std::move(s));
    }
    return {};
}

[[nodiscard]] inline ModuleLoadError load_conversations(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedConversationSentence>& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            return load_conversations(reader, out, src);
        });
}

} // namespace wse::game
