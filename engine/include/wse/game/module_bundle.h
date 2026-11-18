// Clean-room v1.1.5.6 full module bundle loader: coordinates world, asset,
// script, trigger, string-bank, and Data/ files via wse::platform::Vfs.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/core/encoding.h"
#include "wse/game/module_asset_loaders.h"
#include "wse/game/module_loader_common.h"
#include "wse/game/module_mission_loaders.h"
#include "wse/game/module_world_loaders.h"
#include "wse/game/script.h"
#include "wse/game/trigger.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"
#include "wse/vm/wse_state.h"

namespace wse::game {

// Owns UTF-16 backing strings for str_store_string / quick_string lookups
// and keeps stable u16string_view spans bound to VmContext / WseStringBanks.
struct ModuleStringStorage {
    std::vector<std::u16string> global_backing{};
    std::vector<std::u16string> local_backing{};
    std::vector<std::u16string_view> global_views{};
    std::vector<std::u16string_view> local_views{};

    // Move-only: the views point into this object's backing strings, so a
    // member-wise copy would alias (and outlive) the source. Moving a
    // std::vector keeps its elements in place, so moved views stay valid.
    ModuleStringStorage() = default;
    ModuleStringStorage(const ModuleStringStorage&) = delete;
    ModuleStringStorage& operator=(const ModuleStringStorage&) = delete;
    ModuleStringStorage(ModuleStringStorage&&) noexcept = default;
    ModuleStringStorage& operator=(ModuleStringStorage&&) noexcept = default;

    void clear() {
        global_views.clear();
        local_views.clear();
        global_backing.clear();
        local_backing.clear();
    }

    void rebuild_views() {
        global_views.clear();
        global_views.reserve(global_backing.size());
        for (const auto& s : global_backing) {
            global_views.emplace_back(s);
        }
        local_views.clear();
        local_views.reserve(local_backing.size());
        for (const auto& s : local_backing) {
            local_views.emplace_back(s);
        }
    }

    void set_strings(const std::vector<LoadedStringEntry>& globals,
                     const std::vector<LoadedStringEntry>& locals) {
        clear();
        global_backing.reserve(globals.size());
        for (const auto& entry : globals) {
            global_backing.push_back(core::wse_utf8_to_utf16(entry.text));
        }
        local_backing.reserve(locals.size());
        for (const auto& entry : locals) {
            local_backing.push_back(core::wse_utf8_to_utf16(entry.text));
        }
        rebuild_views();
    }

    void bind(vm::VmContext& ctx) const noexcept {
        ctx.globalStrings = global_views;
        ctx.localStrings = local_views;
    }

    void bind(vm::WseStringBanks& banks) const noexcept {
        banks.global = global_views;
        banks.local = local_views;
    }
};

struct ModuleBundle {
    ScriptManager scripts{};
    TriggerManager game_triggers{};
    SimpleTriggerManager simple_triggers{};
    ModuleIniData module_ini{};
    ModuleStringStorage string_storage{};
    std::vector<LoadedStringEntry> string_entries{};
    std::vector<LoadedStringEntry> quick_string_entries{};
    std::vector<LoadedFactionMeta> faction_meta{};
    std::vector<LoadedTroopMeta> troop_meta{};
    std::vector<LoadedItemTriggers> item_triggers{};
    std::vector<LoadedPartyMeta> party_meta{};
    std::vector<LoadedSiteMeta> site_meta{};
    std::vector<LoadedMesh> meshes{};
    std::vector<LoadedAction> actions{};
    std::vector<LoadedSkin> skins{};
    LoadedSoundBank sound_bank{};
    std::vector<LoadedMusicTrack> music_tracks{};
    std::vector<LoadedMapIcon> map_icons{};
    std::vector<LoadedSceneProp> scene_props{};
    std::vector<LoadedMenu> menus{};
    std::vector<LoadedPresentation> presentations{};
    std::vector<LoadedMissionTemplateRecord> mission_templates{};
    std::vector<LoadedTableauMaterial> tableau_materials{};
    std::vector<LoadedConversationSentence> conversations{};
    std::vector<LoadedParticleSystem> particle_systems{};
    std::vector<LoadedPostEffect> post_effects{};
    std::vector<LoadedFloraKind> flora_kinds{};
    std::vector<LoadedGroundSpec> ground_specs{};
    std::vector<LoadedSkybox> skyboxes{};
    std::vector<LoadedItemModifier> item_modifiers{};
    std::vector<std::string> loaded_files{};
    std::vector<std::string> skipped_files{};

    void clear() {
        scripts = {};
        game_triggers = {};
        simple_triggers = {};
        module_ini = {};
        string_storage.clear();
        string_entries.clear();
        quick_string_entries.clear();
        faction_meta.clear();
        troop_meta.clear();
        item_triggers.clear();
        party_meta.clear();
        site_meta.clear();
        meshes.clear();
        actions.clear();
        skins.clear();
        sound_bank = {};
        music_tracks.clear();
        map_icons.clear();
        scene_props.clear();
        menus.clear();
        presentations.clear();
        mission_templates.clear();
        tableau_materials.clear();
        conversations.clear();
        particle_systems.clear();
        post_effects.clear();
        flora_kinds.clear();
        ground_specs.clear();
        skyboxes.clear();
        item_modifiers.clear();
        loaded_files.clear();
        skipped_files.clear();
    }
};

struct ModuleLoadOptions {
    std::string native_dir = "Modules/Native";
    std::string root_data_dir = "Data";
    std::string config_ini_path{};
    bool require_core_files = true;
    bool seed_troop_xp = false;
    std::uint32_t troop_xp_seed = 0;
};

[[nodiscard]] inline ModuleLoadError load_scripts(platform::Vfs& vfs,
                                                  std::string_view path,
                                                  ScriptManager& out) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            const vm::ReadError err = read_script_manager(reader, out);
            return err == vm::ReadError::none ? ModuleLoadError{}
                                              : make_read_error(src, err);
        });
}

[[nodiscard]] inline ModuleLoadError load_triggers(
    platform::Vfs& vfs,
    std::string_view path,
    TriggerManager& out,
    std::span<const std::int64_t> timers_total = {}) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            ModuleLoadError hdr_err{};
            if (!skip_header_tokens(reader, 3, src, hdr_err)) {
                return hdr_err;
            }
            constexpr std::array<std::int64_t, 3> kDefaultTimers{};
            const std::span<const std::int64_t> timers =
                timers_total.empty()
                    ? std::span<const std::int64_t>(kDefaultTimers)
                    : timers_total;
            const vm::ReadError err =
                read_trigger_manager(reader, out, "Game", timers);
            return err == vm::ReadError::none ? ModuleLoadError{}
                                              : make_read_error(src, err);
        });
}

[[nodiscard]] inline ModuleLoadError load_simple_triggers(
    platform::Vfs& vfs,
    std::string_view path,
    SimpleTriggerManager& out,
    std::span<const std::int64_t> timers_total = {}) {
    return detail::load_vfs_with_reader(
        vfs, path, [&](io::TokenReader& reader, std::string_view src) {
            ModuleLoadError hdr_err{};
            if (!skip_header_tokens(reader, 3, src, hdr_err)) {
                return hdr_err;
            }
            constexpr std::array<std::int64_t, 3> kDefaultTimers{};
            const std::span<const std::int64_t> timers =
                timers_total.empty()
                    ? std::span<const std::int64_t>(kDefaultTimers)
                    : timers_total;
            const vm::ReadError err =
                read_simple_trigger_manager(reader, out, "Simple", timers);
            return err == vm::ReadError::none ? ModuleLoadError{}
                                              : make_read_error(src, err);
        });
}

namespace detail {

template <class Fn>
[[nodiscard]] inline ModuleLoadError load_step_if_present(
    platform::Vfs& vfs,
    std::string_view path,
    bool required,
    ModuleBundle& bundle,
    Fn&& fn) {
    if (!vfs.is_file(path)) {
        if (required) {
            return make_load_error(path, "unable to open file");
        }
        bundle.skipped_files.emplace_back(path);
        return {};
    }
    ModuleLoadError err = fn(path);
    if (!err.ok) {
        return err;
    }
    bundle.loaded_files.emplace_back(path);
    return {};
}

template <class Fn>
[[nodiscard]] inline ModuleLoadError load_data_step(
    platform::Vfs& vfs,
    std::string_view module_dir,
    std::string_view root_data_dir,
    std::string_view rel_name,
    ModuleBundle& bundle,
    Fn&& fn) {
    const std::string mod_data =
        vfs_join_path(vfs_join_path(module_dir, "Data"), rel_name);
    if (vfs.is_file(mod_data)) {
        ModuleLoadError err = fn(mod_data);
        if (!err.ok) {
            return err;
        }
        bundle.loaded_files.push_back(mod_data);
        return {};
    }
    if (!root_data_dir.empty()) {
        const std::string root_data = vfs_join_path(root_data_dir, rel_name);
        if (vfs.is_file(root_data)) {
            ModuleLoadError err = fn(root_data);
            if (!err.ok) {
                return err;
            }
            bundle.loaded_files.push_back(root_data);
            return {};
        }
    }
    bundle.skipped_files.push_back(mod_data);
    return {};
}

} // namespace detail

namespace detail {

// Drops everything a previous load_module() derived from module files, so a
// file that is optional or absent in the next module cannot leak through.
inline void reset_module_world_state(WseWorldState& world) {
    world.skills = {};
    world.items.clear();
    world.party_templates.clear();
    world.troops.clear();
    world.factions.clear();
    world.quests.clear();
    world.sites.clear();
    world.parties.clear();
    world.next_party_key = 0;
    world.clear_mission_entities();
    world.mission_templates.clear();
    world.info_pages.clear();
    world.mesh_names.clear();
    world.module_settings.clear();
    world.game_variables.clear();
    world.num_skins = 2; // WseWorldState default (g_faceGenerator.m_numSkins)
    world.num_particle_systems = 0;
    world.misc.num_actions = 0;
    world.misc.num_music_tracks = 0;
    world.misc.num_post_effects = 0;
    world.misc.num_map_icons = 0;
    world.misc.num_scene_props = 0;
    world.misc.num_menus = 0;
    world.misc.num_tableau_materials = 0;
    world.m5.num_sounds = 0;
    world.m5.skybox_fog_colors.fill(0U);
    world.m6.sites.clear();
    world.m5.enqueued_windows.clear();
    world.m5.active_window = 0;
    world.m6.ui.active_window_id = -1;
}

} // namespace detail

// Loads a full or partial Warband/WSE2 module under `module_dir` into `ctx`
// and `bundle`, binding string banks and `ctx.scripts` on success. Module-derived
// world state from an earlier load is reset first. `ctx` keeps a pointer to
// `bundle.scripts` and spans into `bundle.string_storage`, so `bundle` must
// outlive every use of `ctx` and must not be moved while bound.
[[nodiscard]] inline ModuleLoadError load_module(
    platform::Vfs& vfs,
    std::string_view module_dir,
    vm::VmContext& ctx,
    ModuleBundle& bundle,
    const ModuleLoadOptions& options = {}) {
    bundle.clear();
    ctx.scripts = nullptr;
    ctx.globalStrings = {};
    ctx.localStrings = {};
    detail::reset_module_world_state(ctx.world);

    const bool req = options.require_core_files;
    const auto mod_path = [&](std::string_view file) {
        return vfs_join_path(module_dir, file);
    };

    ModuleLoadError err = detail::load_step_if_present(
        vfs, mod_path("module.ini"), req, bundle, [&](std::string_view) {
            return load_module_ini(vfs, module_dir, bundle.module_ini);
        });
    if (!err.ok) return err;

    const std::string cfg_path = options.config_ini_path.empty()
                                     ? mod_path("module_config.ini")
                                     : options.config_ini_path;
    if (vfs.is_file(cfg_path)) {
        load_module_settings(vfs, cfg_path, ctx.world);
        bundle.loaded_files.push_back(cfg_path);
    } else {
        bundle.skipped_files.push_back(cfg_path);
    }

    load_game_variables(vfs, module_dir, options.native_dir, ctx.world);
    const std::string gv_path = mod_path("game_variables.txt");
    if (vfs.is_file(gv_path)) {
        bundle.loaded_files.push_back(gv_path);
    } else {
        bundle.skipped_files.push_back(gv_path);
    }

    err = detail::load_step_if_present(
        vfs, mod_path("skills.txt"), req, bundle, [&](std::string_view p) {
            return load_skills(vfs, p, ctx.world);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("particle_systems.txt"), req, bundle,
        [&](std::string_view p) {
            return load_particle_systems(vfs, p, ctx.world,
                                         &bundle.particle_systems);
        });
    if (!err.ok) return err;

    const std::string postfx_path = mod_path("postfx.txt");
    if (vfs.is_file(postfx_path)) {
        err = load_post_effects(vfs, postfx_path, ctx.world,
                                &bundle.post_effects);
        if (!err.ok) return err;
        bundle.loaded_files.push_back(postfx_path);
    } else {
        LoadedPostEffect def{};
        def.id = "pfx_default";
        bundle.post_effects.push_back(std::move(def));
        ctx.world.misc.num_post_effects = 1;
        bundle.skipped_files.push_back(postfx_path);
    }

    err = detail::load_step_if_present(
        vfs, mod_path("sounds.txt"), req, bundle, [&](std::string_view p) {
            return load_sounds(vfs, p, ctx.world, &bundle.sound_bank);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("music.txt"), false, bundle, [&](std::string_view p) {
            return load_music(vfs, p, ctx.world, &bundle.music_tracks);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("meshes.txt"), req, bundle, [&](std::string_view p) {
            return load_meshes(vfs, p, ctx.world, &bundle.meshes);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("actions.txt"), req, bundle, [&](std::string_view p) {
            return load_actions(vfs, p, ctx.world, &bundle.actions);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("skins.txt"), req, bundle, [&](std::string_view p) {
            return load_skins(vfs, p, ctx.world, &bundle.skins);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("item_kinds1.txt"), req, bundle, [&](std::string_view p) {
            return load_item_kinds(vfs, p, ctx.world, &bundle.item_triggers);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("factions.txt"), req, bundle, [&](std::string_view p) {
            return load_factions(vfs, p, ctx.world, &bundle.faction_meta);
        });
    if (!err.ok) return err;

    std::uint32_t xp_seed = options.troop_xp_seed;
    std::uint32_t* xp_seed_ptr = options.seed_troop_xp ? &xp_seed : nullptr;
    err = detail::load_step_if_present(
        vfs, mod_path("troops.txt"), req, bundle, [&](std::string_view p) {
            return load_troops(vfs, p, ctx.world, xp_seed_ptr,
                               &bundle.troop_meta);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("party_templates.txt"), req, bundle,
        [&](std::string_view p) {
            return load_party_templates(vfs, p, ctx.world);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("parties.txt"), req, bundle, [&](std::string_view p) {
            return load_parties(vfs, p, ctx.world, &bundle.party_meta);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("scenes.txt"), req, bundle, [&](std::string_view p) {
            return load_sites(vfs, p, ctx.world, &bundle.site_meta);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("quests.txt"), req, bundle, [&](std::string_view p) {
            return load_quests(vfs, p, ctx.world);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("info_pages.txt"), false, bundle,
        [&](std::string_view p) {
            return load_info_pages(vfs, p, ctx.world);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("strings.txt"), req, bundle, [&](std::string_view p) {
            return load_strings(vfs, p, bundle.string_entries);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("quick_strings.txt"), req, bundle,
        [&](std::string_view p) {
            return load_quick_strings(vfs, p, bundle.quick_string_entries);
        });
    if (!err.ok) return err;

    bundle.string_storage.set_strings(bundle.string_entries,
                                      bundle.quick_string_entries);

    err = detail::load_step_if_present(
        vfs, mod_path("scripts.txt"), req, bundle, [&](std::string_view p) {
            return load_scripts(vfs, p, bundle.scripts);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("triggers.txt"), req, bundle, [&](std::string_view p) {
            return load_triggers(vfs, p, bundle.game_triggers,
                                 ctx.timersTotal);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("simple_triggers.txt"), req, bundle,
        [&](std::string_view p) {
            return load_simple_triggers(vfs, p, bundle.simple_triggers,
                                        ctx.timersTotal);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("map_icons.txt"), req, bundle, [&](std::string_view p) {
            return load_map_icons(vfs, p, ctx.world, &bundle.map_icons);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("scene_props.txt"), req, bundle, [&](std::string_view p) {
            return load_scene_props(vfs, p, ctx.world, &bundle.scene_props);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("menus.txt"), req, bundle, [&](std::string_view p) {
            return load_menus(vfs, p, ctx.world, &bundle.menus);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("presentations.txt"), req, bundle,
        [&](std::string_view p) {
            return load_presentations(vfs, p, bundle.presentations);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("mission_templates.txt"), req, bundle,
        [&](std::string_view p) {
            return load_mission_templates(vfs, p, ctx.world, ctx.timersTotal,
                                          &bundle.mission_templates);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("tableau_materials.txt"), req, bundle,
        [&](std::string_view p) {
            return load_tableau_materials(vfs, p, ctx.world,
                                          &bundle.tableau_materials);
        });
    if (!err.ok) return err;

    err = detail::load_step_if_present(
        vfs, mod_path("conversation.txt"), req, bundle,
        [&](std::string_view p) {
            return load_conversations(vfs, p, bundle.conversations);
        });
    if (!err.ok) return err;

    err = detail::load_data_step(
        vfs, module_dir, options.root_data_dir, "item_modifiers.txt", bundle,
        [&](std::string_view p) {
            return load_item_modifiers(vfs, p, bundle.item_modifiers);
        });
    if (!err.ok) return err;

    err = detail::load_data_step(
        vfs, module_dir, options.root_data_dir, "flora_kinds.txt", bundle,
        [&](std::string_view p) {
            return load_flora_kinds(vfs, p, bundle.flora_kinds);
        });
    if (!err.ok) return err;

    err = detail::load_data_step(
        vfs, module_dir, options.root_data_dir, "ground_specs.txt", bundle,
        [&](std::string_view p) {
            return load_ground_specs(vfs, p, bundle.ground_specs);
        });
    if (!err.ok) return err;

    err = detail::load_data_step(
        vfs, module_dir, options.root_data_dir, "skyboxes.txt", bundle,
        [&](std::string_view p) {
            return load_skyboxes(vfs, p, ctx.world, &bundle.skyboxes);
        });
    if (!err.ok) return err;

    bundle.string_storage.bind(ctx);
    ctx.scripts = &bundle.scripts;
    return {};
}

} // namespace wse::game
