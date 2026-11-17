// Clean-room Phase 2H headless Warband/WSE2 module validator CLI.
//
// Loads a user-supplied module directory through HostVfs, runs `game_start`,
// and steps one or more simulated trigger frames while checking for zero VM
// errors and zero unimplemented opcodes.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "wse/game/headless_runner.h"
#include "wse/platform/log.h"
#include "wse/platform/vfs.h"
#include "wse/vm/lua_manager.h"

namespace {

void print_usage(const char* argv0) {
    std::fprintf(stderr,
                 "Usage: %s <warband_root_dir> [module_rel_dir] [options]\n"
                 "Options:\n"
                 "  --frames N             Number of trigger frames to simulate (default: 1)\n"
                 "  --step-ticks T         Engine ticks added per frame (default: 200000 = 2.0s)\n"
                 "  --allow-unimplemented  Count and report unimplemented opcodes without failing the run\n"
                 "  --no-ui                Do not drive notes / presentation windows between frames\n"
                 "  --audio                Mix module audio into a capture device (WAV samples only)\n"
                 "  --preload-audio        With --audio: decode every sample at load\n"
                 "  -h, --help             Show this help\n",
                 argv0 != nullptr ? argv0 : "wse_headless");
}

template <typename Int>
bool parse_int(std::string_view text, Int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

} // namespace

int main(int argc, char** argv) {
    std::string root_arg;
    std::string module_arg = "Modules/Native";
    bool have_root = false;
    bool have_module = false;

    wse::game::HeadlessRunOptions opts{};
    opts.load_options.native_dir = "Modules/Native";
    opts.load_options.root_data_dir = "Data";
    opts.load_options.require_core_files = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--allow-unimplemented") {
            opts.fail_on_unimplemented = false;
            continue;
        }
        if (arg == "--no-ui") {
            opts.drive_ui = false;
            continue;
        }
        if (arg == "--audio") {
            opts.drive_audio = true;
            continue;
        }
        if (arg == "--preload-audio") {
            opts.preload_audio = true;
            continue;
        }
        if (arg == "--frames") {
            if (i + 1 >= argc || !parse_int(argv[++i], opts.trigger_frames)) {
                std::fprintf(stderr, "Invalid --frames value\n");
                return 2;
            }
            continue;
        }
        if (arg == "--step-ticks") {
            if (i + 1 >= argc || !parse_int(argv[++i], opts.frame_ticks)) {
                std::fprintf(stderr, "Invalid --step-ticks value\n");
                return 2;
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "Unknown option: %.*s\n",
                         static_cast<int>(arg.size()), arg.data());
            print_usage(argv[0]);
            return 2;
        }
        if (!have_root) {
            root_arg = std::string(arg);
            have_root = true;
        } else if (!have_module) {
            module_arg = std::string(arg);
            have_module = true;
        } else {
            std::fprintf(stderr, "Unexpected extra positional argument: %.*s\n",
                         static_cast<int>(arg.size()), arg.data());
            return 2;
        }
    }

    if (!have_root) {
        print_usage(argc > 0 ? argv[0] : "wse_headless");
        return 2;
    }

    opts.module_dir = module_arg;

    wse::platform::StderrLog stderr_log;
    opts.log = &stderr_log;

    const std::filesystem::path root_path(root_arg);
    wse::platform::HostVfs vfs(root_path);

    std::printf("Loading module '%s' from '%s'...\n",
                opts.module_dir.c_str(), root_arg.c_str());

    wse::game::HeadlessSession session{};
    session.ctx->luaRuntime = wse::lua::create_lua_runtime();
    session.report =
        wse::game::run_headless_module(vfs, *session.ctx, *session.bundle, opts);
    // Flush sandboxed Lua files while the Vfs is still alive.
    session.ctx->luaRuntime->shutdown();
    const auto& r = session.report;
    const auto& b = *session.bundle;
    const auto& c = *session.ctx;

    if (!r.load_error.ok) {
        std::printf("LOAD FAILED: %s\n", r.load_error.format().c_str());
        return 1;
    }

    std::printf("LOAD OK: loaded_files=%zu skipped_files=%zu\n",
                r.loaded_files, r.skipped_files);
    std::printf("  skills=%zu factions=%zu troops=%zu items=%zu party_templates=%zu parties=%zu sites=%zu\n",
                c.world.skills.size(), c.world.factions.size(),
                c.world.troops.size(), c.world.items.size(),
                c.world.party_templates.size(), c.world.parties.size(),
                c.world.sites.size());
    std::printf("  quests=%zu info_pages=%zu strings=%zu quick_strings=%zu scripts=%zu triggers=%zu simple_triggers=%zu\n",
                c.world.quests.size(), c.world.info_pages.size(),
                b.string_entries.size(), b.quick_string_entries.size(),
                b.scripts.scripts.size(), b.game_triggers.triggers.size(),
                b.simple_triggers.triggers.size());
    std::printf("  meshes=%zu actions=%zu skins=%zu sounds=%zu music=%zu map_icons=%zu scene_props=%zu menus=%zu presentations=%zu mission_templates=%zu tableaus=%zu dialogs=%zu\n",
                b.meshes.size(), b.actions.size(), b.skins.size(),
                b.sound_bank.sounds.size(), b.music_tracks.size(),
                b.map_icons.size(), b.scene_props.size(),
                b.menus.size(), b.presentations.size(),
                b.mission_templates.size(), b.tableau_materials.size(),
                b.conversations.size());
    std::printf("  data: item_modifiers=%zu flora=%zu ground_specs=%zu skyboxes=%zu\n",
                b.item_modifiers.size(), b.flora_kinds.size(),
                b.ground_specs.size(), b.skyboxes.size());
    std::printf("game_start idx=%d executed=%s ok=%s maxDepth=%d\n",
                r.game_start_script_index,
                r.game_start_executed ? "true" : "false",
                r.game_start_ok ? "true" : "false",
                r.max_observed_depth);
    std::printf("triggers=%zu simple_triggers=%zu frames=%zu triggers_ok=%s simple_triggers_ok=%s parties=%zu\n",
                b.game_triggers.triggers.size(),
                b.simple_triggers.triggers.size(),
                r.frames_executed,
                r.triggers_ok ? "true" : "false",
                r.simple_triggers_ok ? "true" : "false",
                r.party_count);
    std::printf("ui=%s presentations_opened=%zu notes_opened=%zu menus_opened=%zu final_window=%d\n",
                opts.drive_ui ? "on" : "off", r.presentations_opened, r.notes_opened,
                r.menus_opened, r.final_window);
    if (opts.drive_audio) {
        std::printf("audio=on samples_resolved=%zu samples_missing=%zu samples_decoded=%zu "
                    "decode_failures=%zu music_resolved=%zu frames_mixed=%zu\n",
                    r.audio_stats.sample_paths_resolved, r.audio_stats.sample_paths_missing,
                    r.audio_stats.samples_decoded, r.audio_stats.sample_decode_failures,
                    r.audio_stats.music_paths_resolved, r.audio_frames_mixed);
    }
    std::printf("FINAL: ok=%s errorCount=%zu unimplementedCount=%zu uniqueUnimpl=%zu\n",
                r.ok ? "true" : "false", r.error_count, r.unimplemented_count,
                r.unimplemented_opcodes.size());
    if (!r.last_error.empty()) {
        std::printf("  lastError=%s\n", r.last_error.c_str());
    }
    for (const auto& [op, cnt] : r.unimplemented_opcodes) {
        std::printf("  unimplemented op %u x %zu\n", op, cnt);
    }
    return r.ok ? 0 : 1;
}
