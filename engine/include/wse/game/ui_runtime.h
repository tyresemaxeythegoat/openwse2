// Clean-room v1.1.5.5 UI localization, widget properties, presentation runtime,
// and menu/initial/notes window controllers (`docs/PLATFORM_PLAN.md`, P9 Part 2).
//
// Reconstructed from v1.1.5.5 PE + PDB evidence (`mb_warband_wse2_x64.pdb`):
// - mbLanguageManager::getText             (0x14028e300),
//   mbLanguageManager file scan            (0x14028d6f0: languages/<lang>/*.csv)
// - rglReadWidgetProperties                (0x140328680: <prefix>_text_color,
//   _highlight_text_color, _position_x/y, _size_x/y, _text_size_x/y, _text_flags)
// - rglWidget                              (size 632 B),
//   rglWidget::drawText                    (0x14031e5a0),
//   rglWidget::setMeshStretched            (0x14031d310),
//   rglCreateImageWidget                   (0x14031ec20),
//   rglCreateSimpleButtonWidget            (0x14031f110)
// - mbInitialWindow                        (PDB layout/properties via `initial_*` game_variables)
// - mbMenuWindow::open                     (0x140120240),
//   mbMenuWindow::frameMove                (0x140121110)
// - mbPresentationContainer::startPresentation (0x1402097b0),
//   mbPresentationContainer::frameMove         (0x140208ec0),
//   mbPresentationWindow::frameMove            (0x140174ed0)
// - mbNotesWindow::frameMove                 (0x140161520)
// - mbGameScreen::setWindow / openWindow     (0x1400f0c80 / 0x1400f0dd0)
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/asset/font.h"
#include "wse/core/math.h"
#include "wse/game/module_loader_common.h"
#include "wse/game/module_mission_loaders.h"
#include "wse/game/world_state.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"

namespace wse::game {

// Additional screen window identifiers extending `world_subsystem_types.h` (`0x1400ef710`).
inline constexpr std::int32_t kScreenWindowInitial = 0x00;
inline constexpr std::int32_t kScreenWindowOptions = 0x13;
inline constexpr std::int32_t kScreenWindowMessageBox = 0x14;

// Warband UI coordinate extents (`rglWindowManager::render` `0x14032ace0`).
inline constexpr float kUiScreenWidth = 1.0f;
inline constexpr float kUiScreenHeight = 0.75f;

// -----------------------------------------------------------------------------
// Localization table (`languages/<lang>/{ui,uimain,hints,wse2}.csv`)
// -----------------------------------------------------------------------------

class LanguageTable {
public:
    void clear() { entries_.clear(); }

    void parse_csv(std::string_view text) {
        std::size_t pos = 0;
        if (text.size() >= 3U && static_cast<unsigned char>(text[0]) == 0xEFU &&
            static_cast<unsigned char>(text[1]) == 0xBBU &&
            static_cast<unsigned char>(text[2]) == 0xBFU) {
            pos = 3U;
        }
        while (pos < text.size()) {
            std::size_t eol = text.find('\n', pos);
            if (eol == std::string_view::npos) {
                eol = text.size();
            }
            std::string_view line = text.substr(pos, eol - pos);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1U);
            }
            pos = (eol < text.size()) ? (eol + 1U) : text.size();
            if (line.empty()) {
                continue;
            }
            const std::size_t bar = line.find('|');
            if (bar == std::string_view::npos || bar == 0U) {
                continue;
            }
            const std::string key = trim_ascii(line.substr(0U, bar));
            std::string val = decode_escapes(line.substr(bar + 1U));
            if (!key.empty()) {
                entries_[key] = std::move(val);
            }
        }
    }

    bool load_from_vfs(platform::Vfs& vfs,
                       std::string_view language = "en",
                       std::string_view module_rel_dir = {}) {
        clear();
        bool any = false;
        const std::array<std::string_view, 4> kFiles{
            "ui.csv",
            "uimain.csv",
            "hints.csv",
            "wse2.csv",
        };
        auto load_dir = [&](std::string_view prefix) {
            for (const std::string_view fname : kFiles) {
                std::string path;
                if (!prefix.empty()) {
                    path.append(prefix);
                    if (path.back() != '/') {
                        path.push_back('/');
                    }
                }
                path.append("languages/");
                path.append(language);
                path.push_back('/');
                path.append(fname);
                std::vector<std::uint8_t> raw;
                if (vfs.read_file(path, raw)) {
                    parse_csv(std::string_view(reinterpret_cast<const char*>(raw.data()),
                                               raw.size()));
                    any = true;
                }
            }
        };
        load_dir({});
        if (!module_rel_dir.empty()) {
            load_dir(module_rel_dir);
        }
        return any;
    }

    // Resolves `ui_*` key (`mbLanguageManager::getText` `0x14028e300`), falling back cleanly
    // to a human-readable string stripped of the `ui_` prefix when absent.
    [[nodiscard]] std::string get_text(std::string_view key) const {
        const std::string skey(key);
        const auto it = entries_.find(skey);
        if (it != entries_.end()) {
            return it->second;
        }
        if (key.size() > 3U && key.substr(0U, 3U) == "ui_") {
            std::string fallback(key.substr(3U));
            underscores_to_spaces(fallback);
            return fallback;
        }
        return skey;
    }

    [[nodiscard]] bool contains(std::string_view key) const {
        return entries_.find(std::string(key)) != entries_.end();
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    static std::string decode_escapes(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 1U < raw.size() && raw[i + 1U] == 'n') {
                out.push_back('\n');
                ++i;
            } else {
                out.push_back(raw[i]);
            }
        }
        return out;
    }

    std::unordered_map<std::string, std::string> entries_{};
};

// -----------------------------------------------------------------------------
// Layout variable table (`Modules/<mod>/game_variables.txt`)
// -----------------------------------------------------------------------------

class GameVariablesTable {
public:
    void clear() { vars_.clear(); }

    void parse(std::string_view text) {
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t eol = text.find('\n', pos);
            if (eol == std::string_view::npos) {
                eol = text.size();
            }
            std::string_view line = text.substr(pos, eol - pos);
            pos = (eol < text.size()) ? (eol + 1U) : text.size();
            const std::size_t hash = line.find('#');
            if (hash != std::string_view::npos) {
                line = line.substr(0U, hash);
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string_view::npos) {
                continue;
            }
            const std::string key = trim_ascii(line.substr(0U, eq));
            const std::string val = trim_ascii(line.substr(eq + 1U));
            if (!key.empty() && !val.empty()) {
                vars_[key] = val;
            }
        }
    }

    bool load_from_vfs(platform::Vfs& vfs, std::string_view module_rel_dir) {
        clear();
        const std::string path = module_rel_dir.empty()
                                     ? std::string("game_variables.txt")
                                     : vfs_join_path(module_rel_dir, "game_variables.txt");
        std::vector<std::uint8_t> raw;
        if (!vfs.read_file(path, raw)) {
            return false;
        }
        parse(std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
        return true;
    }

    [[nodiscard]] float get_f32(std::string_view key, float fallback) const noexcept {
        const auto it = vars_.find(std::string(key));
        if (it == vars_.end()) {
            return fallback;
        }
        char* endp = nullptr;
        const float v = std::strtof(it->second.c_str(), &endp);
        if (endp == it->second.c_str() || !std::isfinite(v)) {
            return fallback;
        }
        return v;
    }

    [[nodiscard]] std::uint32_t get_u32(std::string_view key, std::uint32_t fallback) const noexcept {
        const auto it = vars_.find(std::string(key));
        if (it == vars_.end()) {
            return fallback;
        }
        char* endp = nullptr;
        const unsigned long v = std::strtoul(it->second.c_str(), &endp, 0);
        if (endp == it->second.c_str()) {
            return fallback;
        }
        return static_cast<std::uint32_t>(v);
    }

    [[nodiscard]] std::size_t size() const noexcept { return vars_.size(); }

private:
    std::unordered_map<std::string, std::string> vars_{};
};

// -----------------------------------------------------------------------------
// Widget tree model (`rglWidget` / `rglContainerWidget` / `rglSimpleButtonWidget`)
// -----------------------------------------------------------------------------

enum class UiWidgetKind : std::uint8_t {
    Container = 0,
    Image = 1,
    SimpleButton = 2,
    GameButton = 3,
    Label = 4,
    SolidPanel = 5,
    Scrollable = 6,
    ProgressBar = 7,
    Slider = 8,
    CheckBox = 9,
    TextBox = 10,
};

struct UiWidget {
    std::string id{};
    UiWidgetKind kind = UiWidgetKind::Container;
    wse::core::WseVec2 pos{};
    wse::core::WseVec2 size{1.0f, 0.75f};
    std::string text{};
    wse::core::WseVec2 text_pos{};
    wse::core::WseVec2 text_scale{0.022f, 0.030f};
    std::uint32_t text_flags = asset::kTextFlagCenterAlign;
    std::uint32_t color_argb = 0xFFFFFFFFU;
    std::uint32_t text_color_argb = 0xFF110000U;
    std::uint32_t highlight_color_argb = 0xFFFFDD00U;
    std::string mesh_name{};
    std::string highlight_mesh_name{};
    std::string material_name{};
    std::int32_t overlay_id = -1;
    std::int32_t action_tag = -1;
    std::int32_t action_param = 0;
    float draw_priority = 0.0f;
    bool visible = true;
    bool enabled = true;
    bool highlighted = false;
    bool pressed = false;
    std::vector<UiWidget> children{};

    // Populates widget geometry and typography from `GameVariablesTable` following
    // `rglReadWidgetProperties` (`0x140328680`: `<prefix>_position_x/y`, `_size_x/y`,
    // `_text_size_x/y`, `_text_flags`, `_text_color`, `_highlight_text_color`).
    void apply_properties(const GameVariablesTable& vars, std::string_view prefix) {
        const auto k = [prefix](std::string_view suffix) {
            std::string s(prefix);
            s.append(suffix);
            return s;
        };
        pos.x = vars.get_f32(k("_position_x"), pos.x);
        pos.y = vars.get_f32(k("_position_y"), pos.y);
        size.x = vars.get_f32(k("_size_x"), size.x);
        size.y = vars.get_f32(k("_size_y"), size.y);
        text_scale.x = vars.get_f32(k("_text_size_x"), text_scale.x);
        text_scale.y = vars.get_f32(k("_text_size_y"), text_scale.y);
        text_flags = vars.get_u32(k("_text_flags"), text_flags);
        text_color_argb = vars.get_u32(k("_text_color"), text_color_argb);
        highlight_color_argb = vars.get_u32(k("_highlight_text_color"), highlight_color_argb);
    }

    [[nodiscard]] bool hit_test(float ui_x,
                                float ui_y,
                                float parent_x = 0.0f,
                                float parent_y = 0.0f) const noexcept {
        if (!visible || !enabled || size.x <= 0.0f || size.y <= 0.0f) {
            return false;
        }
        const float ax = parent_x + pos.x;
        const float ay = parent_y + pos.y;
        return ui_x >= ax && ui_x <= (ax + size.x) && ui_y >= ay && ui_y <= (ay + size.y);
    }
};

enum class UiActionKind : std::uint8_t {
    None = 0,
    Tutorial = 1,
    StartNewGame = 2,
    ResumeGame = 3,
    Multiplayer = 4,
    CustomBattle = 5,
    Options = 6,
    Credits = 7,
    Quit = 8,
    MenuOption = 9,
    OverlayClicked = 10,
    NotesReturn = 11,
    DialogYes = 12,
    DialogNo = 13,
    DialogOk = 14,
};

struct UiEvent {
    UiActionKind kind = UiActionKind::None;
    std::int32_t param = -1;
    std::string widget_id{};
};

// -----------------------------------------------------------------------------
// Window & presentation session (`mbInitialWindow`, `mbMenuWindow`,
// `mbPresentationContainer`, `mbPresentationWindow`, `mbNotesWindow`)
// -----------------------------------------------------------------------------

// Module System `header_presentations.py` `prsntf_manual_end_only` (flags bit `0x2`, copied to
// the container by `mbPresentationContainer::startPresentation` `0x1402097b0`). Retail tests it
// in one place only: `mbConversationWindow::updatePresentations` (`0x1400888c0`), which, when
// a presentation queued by `mbConversationWindow::startPresentation` (`0x1400888a0`) starts,
// ends every other open presentation container except those with this bit. The campaign
// presentation window never reads it (`mbPresentationWindow::frameMove` `0x140174ed0`), and
// no retail window closes a presentation on Escape: modules poll `key_clicked` themselves
// (17 of Native's 49 presentations). It matters once mission-side presentations are driven
// (`STATUS.md` §5).
inline constexpr std::uint32_t kPresentationFlagManualEndOnly = 0x00000002U;

// `prev_window` default for `open_notes_window` / `start_presentation`: return to the
// window that was current when the modal window opened.
inline constexpr std::int32_t kUiReturnToCurrentWindow = std::numeric_limits<std::int32_t>::min();

// Overlay text size per unit of `overlay_set_size`: `overlay_set_size` (925) calls
// `rglWidget::setScale` (`0x14031e7e0`), which sets the text size to `scale * 0.02`
// (`__real@3ca3d70a`) through `rglWidget::setTextSize` (`0x14031d1c0`).
inline constexpr float kUiOverlayTextScale = 0.020f;
// `create_button_overlay` (912, `createButtonOverlay` `0x14020abe0`) sets text flag `0x200`, so
// `rglWidget::setText` (`0x14031e330`) makes the widget's box the laid-out text extent at the
// text size current when the text was set (0.02 at creation); `setScale` later resizes the
// text only. `UiSession` measures with `UiSession::font` at 0.02; without a font it falls back
// to this fixed box (port approximation).
inline constexpr float kUiOverlayButtonWidth = 0.25f;
inline constexpr float kUiOverlayButtonHeight = 0.038f;
// `create_game_button_overlay` (940, `createGameButtonOverlay` `0x14020c3e0`): an
// `rglGameButtonWidget` with `medium_button` / `medium_button_down` meshes and a
// `0.148 x 0.04` box; its scale field starts at that box, and `rglGameButtonWidget::setScale`
// (`0x14029bfb0`) makes the `overlay_set_size` vector the new box (meshes rescale with it).
inline constexpr float kUiGameButtonWidth = 0.148f;
inline constexpr float kUiGameButtonHeight = 0.04f;
// `create_in_game_button_overlay` (941, `createIngameButtonOverlay` `0x14020c7d0`): same widget
// with `mp_inventory_button` for both states and a `0.2123 x 0.04` box.
inline constexpr float kUiInGameButtonWidth = 0.2123f;
inline constexpr float kUiInGameButtonHeight = 0.04f;

// Meshes the built-in windows and button overlays below draw by name (lower case).
inline constexpr std::array<std::string_view, 8> kUiSessionMeshNames{
    "main_menu_nord", "ui_quick_battle_a",  "warrider_logo",      "dlg_button",
    "dlg_button_down", "medium_button",     "medium_button_down", "mp_inventory_button",
};

// Extent of a UI mesh in its own (UI-space) coordinates, used to place mesh overlays.
struct UiMeshBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 1.0f;
    float max_y = 1.0f;
};

class UiSession {
public:
    UiWidget root{};
    LanguageTable lang{};
    GameVariablesTable vars{};
    // Lower-case mesh name -> UI-space extent (filled by the host, e.g. from
    // `platform::collect_ui_mesh_bounds`). Mesh overlays whose mesh is missing here are
    // assumed to span `[0, 1] x [0, 1]` at scale 1.
    std::unordered_map<std::string, UiMeshBounds> mesh_bounds{};
    // Font used to size `create_button_overlay` boxes from their text (not owned; the host
    // keeps it alive, e.g. `platform::LoadedUiAssets::font`). Null keeps the fixed fallback box.
    const asset::FontData* font = nullptr;
    std::string module_name = "Native";
    std::string version_text = "1.174";
    std::int32_t current_window = kScreenWindowInitial;
    std::int32_t source_window = kScreenWindowMap;
    std::int32_t current_menu_index = -1;
    float presentation_elapsed_s = 0.0f;

    // Closes window `0x15` (Notes) or `0x16` (Presentation) back to `source_window`
    // (`mbGameScreen::setWindow` `0x1400f0c80`) so `world.m5.active_window` /
    // `world.m6.ui.active_window_id` never stay wedged (`STATUS.md` §5 item #1).
    // Closing a presentation clears `active_presentation_no` and drops queued `0x0f`
    // entries for that same presentation; closing notes drops queued `0x10` entries.
    // The modal widget tree is replaced by an empty container so it is neither drawn nor
    // clickable afterwards. A no-op when no modal window is open.
    void close_active_modal_window(WseWorldState& world) {
        const std::int32_t closing_win = current_window;
        if (closing_win != kScreenWindowNotes && closing_win != kScreenWindowPresentation) {
            return;
        }
        if (closing_win == kScreenWindowPresentation) {
            const std::int32_t closing_no = presentation_no_;
            world.active_presentation_no = -1;
            std::erase_if(world.m5.enqueued_windows, [closing_no](const auto& entry) {
                return entry.first == kQueuedWindowPresentationType &&
                       (entry.second == closing_no || entry.second < 0);
            });
            presentation_no_ = -1;
            active_triggers_ = SimpleTriggerManager{};
        } else {
            std::erase_if(world.m5.enqueued_windows, [](const auto& entry) {
                return entry.first == kQueuedWindowNotesType;
            });
        }
        world.m5.active_window = source_window;
        world.m6.ui.active_window_id = source_window;
        current_window = source_window;
        presentation_elapsed_s = 0.0f;
        duration_cs_accum_ = 0.0f;
        reset_root("closed");
    }

    // Reconstructs the Native/mod initial title screen (`mbInitialWindow`, laid out via
    // `rglReadWidgetProperties` `0x140328680` and `rglCreateImageWidget` `0x14031ec20` /
    // `rglCreateSimpleButtonWidget` `0x14031f110`):
    // - Full-viewport backdrop `main_menu_nord`
    // - Left parchment column `menu_background` (`ui_quick_battle_a` `[0..0.35] x [0..0.75]`)
    // - Logo (`warrider_logo`), version label, optional module label, and the 8 menu buttons
    void build_initial_window(WseWorldState& world,
                              std::string_view mod_name = "Native",
                              bool has_saves = true,
                              bool can_new_game = true,
                              bool can_tutorial = true,
                              bool can_multiplayer = true) {
        module_name = std::string(mod_name);
        current_window = kScreenWindowInitial;
        world.m5.active_window = kScreenWindowInitial;
        world.m6.ui.active_window_id = kScreenWindowInitial;

        root = UiWidget{};
        root.id = "initial";
        root.kind = UiWidgetKind::Container;
        root.pos = {0.0f, 0.0f};
        root.size = {kUiScreenWidth, kUiScreenHeight};

        // 1. Main menu backdrop (`main_menu_nord`).
        UiWidget backdrop{};
        backdrop.id = "backdrop";
        backdrop.kind = UiWidgetKind::Image;
        backdrop.pos = {0.0f, 0.0f};
        backdrop.size = {kUiScreenWidth, kUiScreenHeight};
        backdrop.mesh_name = "main_menu_nord";
        backdrop.color_argb = 0xFFFFFFFFU;
        root.children.push_back(std::move(backdrop));

        // 2. Left parchment strip (`menu_background` -> `ui_quick_battle_a`).
        UiWidget sidebar{};
        sidebar.id = "menu_background";
        sidebar.kind = UiWidgetKind::Image;
        sidebar.pos = {0.0f, 0.0f};
        sidebar.size = {0.35f, kUiScreenHeight};
        sidebar.mesh_name = "ui_quick_battle_a";
        sidebar.color_argb = 0xFFFFFFFFU;
        root.children.push_back(std::move(sidebar));

        // 3. Game logo (`game_logo` -> `warrider_logo`).
        UiWidget logo{};
        logo.id = "game_logo";
        logo.kind = UiWidgetKind::Image;
        logo.mesh_name = "warrider_logo";
        logo.pos = {0.045f, 0.54f};
        logo.size = {0.20f, 0.07f};
        logo.apply_properties(vars, "initial_game_logo");
        root.children.push_back(std::move(logo));

        // 4. Game version info (`version_info`).
        UiWidget ver{};
        ver.id = "version_info";
        ver.kind = UiWidgetKind::Label;
        ver.pos = {0.18f, 0.525f};
        ver.size = {0.12f, 0.03f};
        ver.text = version_text;
        ver.text_scale = {0.016f, 0.016f};
        ver.text_flags = asset::kTextFlagVCenterOrLeft;
        ver.text_color_argb = 0xFF110000U;
        ver.apply_properties(vars, "initial_version_info");
        root.children.push_back(std::move(ver));

        // 5. Module label (shown when module != "Native").
        if (!module_name.empty() && module_name != "Native") {
            UiWidget mver{};
            mver.id = "module_version_info";
            mver.kind = UiWidgetKind::Label;
            mver.pos = {0.029f, 0.475f};
            mver.size = {0.15f, 0.03f};
            mver.text = module_name;
            mver.text_scale = {0.014f, 0.014f};
            mver.text_flags = asset::kTextFlagRightAlign;
            mver.text_color_argb = 0xFF110000U;
            mver.apply_properties(vars, "initial_module_version_info");
            root.children.push_back(std::move(mver));
        }

        struct InitialBtnSpec {
            bool active;
            const char* id;
            const char* gv_prefix;
            const char* text_key;
            float def_y;
            float def_h;
            UiActionKind action;
        };
        const std::array<InitialBtnSpec, 8> specs{{
            {can_tutorial, "tutorial_button", "initial_tutorial_button", "ui_start_tutorial",
             0.48f, 0.031f, UiActionKind::Tutorial},
            {can_new_game, "start_new_game_button", "initial_start_new_game_button",
             "ui_start_a_new_game", 0.44f, 0.031f, UiActionKind::StartNewGame},
            {has_saves, "resume_game_button", "initial_resume_game_button",
             "ui_restore_a_saved_game", 0.40f, 0.031f, UiActionKind::ResumeGame},
            {can_multiplayer, "multiplayer_button", "initial_multiplayer_button", "ui_multiplayer",
             0.36f, 0.031f, UiActionKind::Multiplayer},
            {true, "custom_battle_button", "initial_custom_battle_button", "ui_custom_battle",
             0.32f, 0.031f, UiActionKind::CustomBattle},
            {true, "options_button", "initial_options_button", "ui_options", 0.28f, 0.031f,
             UiActionKind::Options},
            {true, "credits_button", "initial_credits_button", "ui_credits", 0.24f, 0.031f,
             UiActionKind::Credits},
            {true, "quit_button", "initial_quit_button", "ui_exit_to_windows", 0.20f, 0.025f,
             UiActionKind::Quit},
        }};

        for (const InitialBtnSpec& sp : specs) {
            if (!sp.active) {
                continue;
            }
            UiWidget btn{};
            btn.id = sp.id;
            btn.kind = UiWidgetKind::SimpleButton;
            btn.pos = {0.05f, sp.def_y};
            btn.size = {0.185f, sp.def_h};
            btn.text = lang.get_text(sp.text_key);
            btn.text_scale = {0.022f, 0.030f};
            btn.text_flags = asset::kTextFlagCenterAlign;
            btn.text_color_argb = 0xFF110000U;
            btn.highlight_color_argb = 0xFFFFDD00U;
            btn.action_tag = static_cast<std::int32_t>(sp.action);
            btn.apply_properties(vars, sp.gv_prefix);
            root.children.push_back(std::move(btn));
        }
    }

    // Builds the game menu screen (`mbMenuWindow::open` `0x140120240`) from loaded module
    // menus + dynamic `world.m6.ui.menus` overrides, evaluating item conditions via `world`.
    void build_menu_window(WseWorldState& world,
                           std::int32_t menu_no,
                           std::span<const LoadedMenu> module_menus = {},
                           vm::VmContext* vm_ctx = nullptr) {
        current_window = kScreenWindowMenu;
        current_menu_index = menu_no;
        world.m5.active_window = kScreenWindowMenu;
        world.m6.ui.active_window_id = kScreenWindowMenu;

        root = UiWidget{};
        root.id = "menu";
        root.kind = UiWidgetKind::Container;
        root.pos = {0.0f, 0.0f};
        root.size = {kUiScreenWidth, kUiScreenHeight};

        UiWidget bg{};
        bg.id = "menu_bg";
        bg.kind = UiWidgetKind::Image;
        bg.pos = {0.0f, 0.0f};
        bg.size = {kUiScreenWidth, kUiScreenHeight};
        bg.mesh_name = "main_menu_nord";
        bg.color_argb = 0xFFFFFFFFU;

        std::string body_text;
        const auto dyn_it = world.m6.ui.menus.find(menu_no);
        if (dyn_it != world.m6.ui.menus.end()) {
            const WseM6DynamicMenu& dm = dyn_it->second;
            if (!dm.mesh_name.empty() && dm.mesh_name != "none") {
                bg.mesh_name = dm.mesh_name;
            }
            body_text = dm.text;
        } else if (menu_no >= 0 && static_cast<std::size_t>(menu_no) < module_menus.size()) {
            const LoadedMenu& lm = module_menus[static_cast<std::size_t>(menu_no)];
            if (!lm.mesh_name.empty() && lm.mesh_name != "none") {
                bg.mesh_name = lm.mesh_name;
            }
            // The menu's operations block runs when the menu opens, before its text and
            // option conditions are evaluated (it typically fills `{s1}` / `{reg0}`).
            if (vm_ctx != nullptr && !lm.operations.statements.empty()) {
                vm::StatementBlock ops_blk = lm.operations;
                (void)vm::execute_statement_block(ops_blk, *vm_ctx);
            }
            body_text = lm.text;
        }
        if (vm_ctx != nullptr) {
            body_text = format_text(body_text, *vm_ctx);
        }
        root.children.push_back(std::move(bg));

        const float menu_width = vars.get_f32("game_menu_width", 0.55f);
        UiWidget text_lbl{};
        text_lbl.id = "menu_text";
        text_lbl.kind = UiWidgetKind::Label;
        text_lbl.pos = {0.38f, 0.42f};
        text_lbl.size = {menu_width, 0.25f};
        text_lbl.text = body_text;
        text_lbl.text_scale = {0.020f, 0.020f};
        text_lbl.text_flags = asset::kTextFlagBoxWrap;
        text_lbl.text_color_argb = 0xFFFFFFFFU;
        root.children.push_back(std::move(text_lbl));

        std::int32_t visible_slot = 0;
        const auto append_option = [&](std::int32_t item_idx, std::string_view label) {
            const float y = 0.36f - static_cast<float>(visible_slot++) * 0.042f;
            UiWidget btn{};
            btn.id = "menu_item_" + std::to_string(item_idx);
            btn.kind = UiWidgetKind::SimpleButton;
            btn.pos = {0.38f, y};
            btn.size = {0.42f, 0.032f};
            btn.mesh_name = "dlg_button";
            btn.highlight_mesh_name = "dlg_button_down";
            btn.text = (vm_ctx != nullptr) ? format_text(label, *vm_ctx) : std::string(label);
            btn.text_scale = {0.020f, 0.020f};
            btn.text_flags = asset::kTextFlagCenterAlign;
            btn.text_color_argb = 0xFFFFFFFFU;
            btn.highlight_color_argb = 0xFFFFDD00U;
            btn.action_tag = static_cast<std::int32_t>(UiActionKind::MenuOption);
            btn.action_param = item_idx;
            root.children.push_back(std::move(btn));
        };

        if (dyn_it != world.m6.ui.menus.end()) {
            std::int32_t idx = 0;
            for (const WseM6DynamicMenuItem& item : dyn_it->second.items) {
                append_option(idx++, item.text);
            }
        } else if (menu_no >= 0 && static_cast<std::size_t>(menu_no) < module_menus.size()) {
            const LoadedMenu& lm = module_menus[static_cast<std::size_t>(menu_no)];
            for (std::size_t i = 0; i < lm.options.size(); ++i) {
                const LoadedMenuOption& opt = lm.options[i];
                if (vm_ctx != nullptr && !opt.condition.statements.empty()) {
                    vm::StatementBlock cond_blk = opt.condition;
                    if (!vm::execute_statement_block(cond_blk, *vm_ctx)) {
                        continue;
                    }
                }
                append_option(static_cast<std::int32_t>(i), opt.text);
            }
        }
    }

    // Executes the consequence block for menu option `option_idx` on `menu_no`.
    static bool execute_menu_option(vm::VmContext& vm_ctx,
                                    std::int32_t menu_no,
                                    std::int32_t option_idx,
                                    std::span<const LoadedMenu> module_menus = {}) {
        if (menu_no < 0 || option_idx < 0) {
            return false;
        }
        const auto dyn_it = vm_ctx.world.m6.ui.menus.find(menu_no);
        if (dyn_it != vm_ctx.world.m6.ui.menus.end()) {
            if (static_cast<std::size_t>(option_idx) >= dyn_it->second.items.size()) {
                return false;
            }
            const std::int32_t script_no =
                dyn_it->second.items[static_cast<std::size_t>(option_idx)].consequence_script_no;
            if (script_no >= 0 && vm_ctx.scripts != nullptr) {
                return vm::execute_script(*vm_ctx.scripts, script_no, vm_ctx);
            }
            return true;
        }
        if (static_cast<std::size_t>(menu_no) >= module_menus.size()) {
            return false;
        }
        const LoadedMenu& lm = module_menus[static_cast<std::size_t>(menu_no)];
        if (static_cast<std::size_t>(option_idx) >= lm.options.size()) {
            return false;
        }
        vm::StatementBlock cons_blk = lm.options[static_cast<std::size_t>(option_idx)].consequence;
        return vm::execute_statement_block(cons_blk, vm_ctx);
    }

    // Opens the notes screen (`mbNotesWindow` `0x1401614d0`) remembering `prev_window`
    // so Return / Escape closes `0x15` cleanly (`STATUS.md` §5 item #1).
    void open_notes_window(WseWorldState& world,
                           std::string_view title,
                           std::string_view body,
                           std::int32_t prev_window = kUiReturnToCurrentWindow) {
        source_window = resolve_prev_window(prev_window);
        current_window = kScreenWindowNotes;
        world.m5.active_window = kScreenWindowNotes;
        world.m6.ui.active_window_id = kScreenWindowNotes;

        reset_root("notes");

        UiWidget title_lbl{};
        title_lbl.id = "notes_title";
        title_lbl.kind = UiWidgetKind::Label;
        title_lbl.pos = {0.03f, 0.67f};
        title_lbl.size = {0.615f, 0.08f};
        title_lbl.text = std::string(title);
        title_lbl.text_scale = {0.025f, 0.025f};
        title_lbl.text_flags = asset::kTextFlagCenterAlign;
        title_lbl.text_color_argb = 0xFF000000U;
        title_lbl.apply_properties(vars, "note_t_title");
        root.children.push_back(std::move(title_lbl));

        UiWidget data_lbl{};
        data_lbl.id = "notes_data";
        data_lbl.kind = UiWidgetKind::Label;
        data_lbl.pos = {0.03f, 0.12f};
        data_lbl.size = {0.615f, 0.52f};
        data_lbl.text = std::string(body);
        data_lbl.text_scale = {0.020f, 0.020f};
        data_lbl.text_flags = asset::kTextFlagBoxWrap;
        data_lbl.text_color_argb = 0xFF000000U;
        data_lbl.apply_properties(vars, "note_t_data");
        root.children.push_back(std::move(data_lbl));

        UiWidget ret_btn{};
        ret_btn.id = "notes_return_button";
        ret_btn.kind = UiWidgetKind::SimpleButton;
        ret_btn.pos = {0.72f, 0.03f};
        ret_btn.size = {0.20f, 0.038f};
        ret_btn.mesh_name = "dlg_button";
        ret_btn.highlight_mesh_name = "dlg_button_down";
        ret_btn.text = lang.get_text("ui_return");
        ret_btn.text_scale = {0.020f, 0.020f};
        ret_btn.text_flags = asset::kTextFlagCenterAlign;
        ret_btn.text_color_argb = 0xFFFFFFFFU;
        ret_btn.action_tag = static_cast<std::int32_t>(UiActionKind::NotesReturn);
        root.children.push_back(std::move(ret_btn));
    }

    // Starts `presentation_no` (`mbPresentationContainer::startPresentation` `0x1402097b0`):
    // clears the previous presentation's overlays, arms the duration countdown in
    // `world.presentation_duration` (1/100 s, written by `presentation_set_duration` 902),
    // fires `-60` (`ti_on_presentation_load`), and syncs `world.overlays` into `root`.
    bool start_presentation(WseWorldState& world,
                            vm::VmContext& vm_ctx,
                            std::int32_t presentation_no,
                            std::span<const LoadedPresentation> presentations,
                            float default_duration_s = 100.0f,
                            std::int32_t prev_window = kUiReturnToCurrentWindow) {
        if (presentation_no < 0 ||
            static_cast<std::size_t>(presentation_no) >= presentations.size()) {
            return false;
        }
        source_window = resolve_prev_window(prev_window);
        default_duration_s_ = default_duration_s;
        begin_presentation(world, vm_ctx, presentation_no, presentations);
        return follow_presentation_state(world, vm_ctx, presentations);
    }

    // Advances presentation state by `dt_s`: counts `world.presentation_duration` down,
    // fires `-61` (`ti_on_presentation_run`, trigger param 1 = elapsed ms), re-syncs
    // `world.overlays`, and closes window `0x16` once the duration runs out
    // (`mbPresentationWindow::frameMove` `0x140174ed0`, `mbPresentationContainer::frameMove`
    // `0x140208ec0`). Returns whether the presentation is still open.
    bool step_presentation(WseWorldState& world,
                           vm::VmContext& vm_ctx,
                           float dt_s,
                           std::span<const LoadedPresentation> presentations) {
        if (current_window != kScreenWindowPresentation || presentation_no_ < 0) {
            return false;
        }
        const float dt = (std::isfinite(dt_s) && dt_s > 0.0f) ? dt_s : 0.0f;
        presentation_elapsed_s += dt;
        duration_cs_accum_ += dt * 100.0f;
        const float whole_cs = std::floor(duration_cs_accum_);
        duration_cs_accum_ -= whole_cs;
        if (whole_cs > 0.0f && world.presentation_duration > 0) {
            const float left = static_cast<float>(world.presentation_duration) - whole_cs;
            world.presentation_duration = (left > 0.0f) ? static_cast<std::int32_t>(left) : 0;
        }
        if (!follow_presentation_state(world, vm_ctx, presentations)) {
            return false;
        }
        const std::array<std::int64_t, 1> params{
            static_cast<std::int64_t>(presentation_elapsed_s * 1000.0f)};
        dispatch_presentation_trigger(world, vm_ctx, -61, params);
        return follow_presentation_state(world, vm_ctx, presentations);
    }

    // Result of one `drive_frame` call.
    struct FrameReport {
        bool opened_presentation = false;  // window 0x16 opened this frame (`-60` fired)
        bool opened_notes = false;         // window 0x15 opened this frame
        bool closed_for_mission = false;   // a modal window was dropped because a scene is up
        bool presentation_open = false;    // window 0x16 is open after this frame
    };

    // Per-frame runtime glue (port infrastructure, no single retail counterpart). Call once per
    // frame after `WseWorldState::step_world_frame`, passing `world.m5.active_window` as it was
    // before that call:
    // - campaign frames: when `consume_campaign_queued_window` (`mbMapWindow::frameMove`
    //   `0x140117f90`) opened window `0x16`, starts `world.active_presentation_no`
    //   (`start_presentation`); when it opened `0x15`, opens the notes window for
    //   `m5.screen_note_kind` / `m5.screen_note_object` (`change_screen_notes` `0x1401dc108`);
    //   otherwise steps an open presentation (`-61`, duration countdown, auto-close). A
    //   presentation does not also step on the frame it opens.
    // - mission frames (`lighting.has_scene`): `step_world_frame` has already retired the
    //   campaign windows, so an open notes / presentation window is dropped here without writing
    //   a campaign window back. Mission-side presentations (`start_presentation` in a scene,
    //   which queues nothing) are not driven yet (`STATUS.md` §5).
    FrameReport drive_frame(WseWorldState& world,
                            vm::VmContext& vm_ctx,
                            float dt_s,
                            std::span<const LoadedPresentation> presentations,
                            std::int32_t window_before_frame) {
        FrameReport rep{};
        const bool modal_open =
            current_window == kScreenWindowNotes || current_window == kScreenWindowPresentation;
        if (world.lighting.has_scene) {
            if (modal_open) {
                const std::int32_t m5_win = world.m5.active_window;
                const std::int32_t ui_win = world.m6.ui.active_window_id;
                close_active_modal_window(world);
                world.m5.active_window = m5_win;
                world.m6.ui.active_window_id = ui_win;
                current_window = -1;
                rep.closed_for_mission = true;
            }
            return rep;
        }

        const std::int32_t win = world.m5.active_window;
        // Return target for a window opened this frame: the window that was up before the
        // queue was consumed, or (from another modal window) wherever that one returns to.
        std::int32_t prev = kScreenWindowMap;
        if (modal_open) {
            prev = kUiReturnToCurrentWindow;
        } else if (window_before_frame >= 0 && window_before_frame != kScreenWindowNotes &&
                   window_before_frame != kScreenWindowPresentation) {
            prev = window_before_frame;
        }

        if (win == kScreenWindowPresentation && current_window != kScreenWindowPresentation) {
            const std::int32_t back = resolve_prev_window(prev);
            if (start_presentation(world, vm_ctx, world.active_presentation_no, presentations,
                                   100.0f, prev)) {
                rep.opened_presentation = true;
            } else if (current_window != kScreenWindowPresentation) {
                // Unknown presentation number: nothing opens, so the queued transition must
                // not leave window 0x16 as the active window.
                world.active_presentation_no = -1;
                world.m5.active_window = back;
                world.m6.ui.active_window_id = back;
            }
        } else if (win == kScreenWindowNotes && current_window != kScreenWindowNotes) {
            if (current_window == kScreenWindowPresentation) {
                // The notes window replaces the presentation; return where it would have.
                close_active_modal_window(world);
                prev = kUiReturnToCurrentWindow;
            }
            std::string title;
            std::string body;
            describe_note_target(world, world.m5.screen_note_kind, world.m5.screen_note_object,
                                 title, body);
            open_notes_window(world, title, body, prev);
            rep.opened_notes = true;
        } else if (current_window == kScreenWindowPresentation) {
            (void)step_presentation(world, vm_ctx, dt_s, presentations);
        }
        rep.presentation_open = current_window == kScreenWindowPresentation;
        return rep;
    }

    // Title and body text for the notes window of `change_screen_notes` kind `kind` (1 troop,
    // 2 faction, 3 party, 4 quest, 5 info page). Port approximation of `mbNotesWindow`'s
    // content (`STATUS.md` §5): the object's name / title, then its description text (quest,
    // info page) and its non-empty note slots, one per paragraph.
    static void describe_note_target(const WseWorldState& world,
                                     std::int32_t kind,
                                     std::int32_t object,
                                     std::string& title,
                                     std::string& body) {
        title.clear();
        body.clear();
        if (object < 0) {
            return;
        }
        const auto idx = static_cast<std::size_t>(object);
        const auto add_para = [&body](std::string_view text) {
            if (text.empty()) {
                return;
            }
            if (!body.empty()) {
                body += "\n\n";
            }
            body += text;
        };
        const auto add_notes = [&](const auto& notes) {
            for (const auto& note : notes) {
                add_para(u16_to_utf8(note.text));
            }
        };
        switch (kind) {
        case 1:
            if (idx < world.troops.size()) {
                title = world.troops[idx].name;
                add_notes(world.troops[idx].notes);
            }
            break;
        case 2:
            if (idx < world.factions.size()) {
                title = world.factions[idx].name;
                add_notes(world.factions[idx].notes);
            }
            break;
        case 3:
            if (idx < world.parties.size()) {
                title = world.parties[idx].name;
                add_notes(world.parties[idx].notes);
            }
            break;
        case 4:
            if (idx < world.quests.size()) {
                title = world.quests[idx].title;
                add_para(world.quests[idx].text);
                add_notes(world.quests[idx].notes);
            }
            break;
        case 5:
            if (idx < world.info_pages.size()) {
                const WseInfoPage& page = world.info_pages[idx];
                title = page.title;
                add_para(page.text);
                for (const WseNoteEntry& note : page.notes) {
                    if (note.available) {
                        add_para(u16_to_utf8(note.text));
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    // Rebuilds overlay widgets inside `root` from `world.overlays` & `world.m6.ui.overlay_ext`
    // (`create_*_overlay` opcodes 910..953). `overlay_set_size` is a scale (1.0 = default
    // size): it scales overlay text and the mesh / button extents. Overlay colours are
    // `0xRRGGBB` (`overlay_set_color` 921) with alpha from `overlay_set_alpha` 922.
    void sync_overlays_from_world(const WseWorldState& world) {
        if (current_window != kScreenWindowPresentation) {
            return;
        }
        std::unordered_map<std::int32_t, std::pair<bool, bool>> prev_state;
        for (const UiWidget& old_w : root.children) {
            if (old_w.overlay_id >= 0) {
                prev_state[old_w.overlay_id] = {old_w.highlighted, old_w.pressed};
            }
        }
        reset_root("presentation");

        for (std::size_t i = 0; i < world.overlays.size(); ++i) {
            const WsePresentationOverlay& ov = world.overlays[i];
            if (!ov.visible) {
                continue;
            }
            UiWidget w{};
            w.overlay_id = static_cast<std::int32_t>(i);
            const auto st_it = prev_state.find(w.overlay_id);
            if (st_it != prev_state.end()) {
                w.highlighted = st_it->second.first;
                w.pressed = st_it->second.second;
            }
            w.id = "overlay_" + std::to_string(i);
            const float scale_x = std::isfinite(ov.size.x) ? std::max(0.0f, ov.size.x) : 1.0f;
            const float scale_y = std::isfinite(ov.size.y) ? std::max(0.0f, ov.size.y) : 1.0f;
            const bool has_area = ov.m5.area_size.x > 0.0f && ov.m5.area_size.y > 0.0f;
            w.pos = {ov.position.x, ov.position.y};
            w.size = has_area ? wse::core::WseVec2{ov.m5.area_size.x, ov.m5.area_size.y}
                              : wse::core::WseVec2{0.0f, 0.0f};
            w.text = u16_to_utf8(ov.text);
            w.text_scale = {kUiOverlayTextScale * scale_x, kUiOverlayTextScale * scale_y};
            const std::uint32_t base = compose_argb(ov.color, ov.alpha);
            w.text_color_argb = base;
            w.color_argb = base;
            w.highlight_color_argb = compose_argb(ov.m5.highlight_color, ov.m5.highlight_alpha);
            w.material_name = ov.m5.material_name;
            if (ov.m5.mesh_no >= 0 &&
                static_cast<std::size_t>(ov.m5.mesh_no) < world.mesh_names.size()) {
                w.mesh_name = world.mesh_names[static_cast<std::size_t>(ov.m5.mesh_no)];
            }
            if (ov.m5.pressed_mesh_no >= 0 &&
                static_cast<std::size_t>(ov.m5.pressed_mesh_no) < world.mesh_names.size()) {
                w.highlight_mesh_name =
                    world.mesh_names[static_cast<std::size_t>(ov.m5.pressed_mesh_no)];
            }

            // Overlay types as stored by the create opcodes: 0 mesh (911/939), 1 text (910)
            // or text button (912/940/941, `m5.is_button`), 2 slider/number box, 3 progress,
            // 4 image button / check box (913/918/938), 5 combo, 6/7 text boxes.
            const bool text_button = (ov.type == 1 && ov.m5.is_button);
            const bool mesh_button = (ov.type == 4);
            if (ov.type == 1) {
                // `create_text_overlay` keeps its `tf_*` flags in `val`; buttons in `widget_flags`.
                w.text_flags = text_button ? ov.m5.widget_flags : static_cast<std::uint32_t>(ov.val);
            } else {
                w.text_flags = ov.m5.widget_flags;
            }
            if (text_button) {
                w.kind = UiWidgetKind::GameButton;
                const std::int32_t op = ov.m5.button_opcode;
                if (op == 940 || op == 941) {
                    // `rglGameButtonWidget`: centred text at the default size (its `setScale`
                    // leaves the text size alone), mesh-backed box.
                    w.text_scale = {kUiOverlayTextScale, kUiOverlayTextScale};
                    w.mesh_name = (op == 940) ? "medium_button" : "mp_inventory_button";
                    w.highlight_mesh_name = (op == 940) ? "medium_button_down" : "mp_inventory_button";
                }
                if (!has_area) {
                    if (op == 940 || op == 941) {
                        const bool game = (op == 940);
                        w.size = ov.m5.size_set
                                     ? wse::core::WseVec2{scale_x, scale_y}
                                     : wse::core::WseVec2{game ? kUiGameButtonWidth : kUiInGameButtonWidth,
                                                          game ? kUiGameButtonHeight
                                                               : kUiInGameButtonHeight};
                    } else if (font != nullptr && !w.text.empty()) {
                        // Box = text extent at the creation text size (see
                        // `kUiOverlayButtonWidth`).
                        const auto [tw, th] = font->measure_text(
                            w.text, 0.0f, kUiOverlayTextScale, kUiOverlayTextScale, w.text_flags);
                        w.size = {tw, th};
                    } else {
                        w.size = {kUiOverlayButtonWidth * scale_x, kUiOverlayButtonHeight * scale_y};
                    }
                }
            } else if (mesh_button || (ov.type == 0 && !w.mesh_name.empty())) {
                w.kind = mesh_button ? UiWidgetKind::GameButton : UiWidgetKind::Image;
                if (!has_area && !w.mesh_name.empty()) {
                    // Place the mesh at its native UI-space extent times the scale, which is
                    // what `make_stretched_model_matrix` reproduces from this box.
                    UiMeshBounds b{};
                    const auto bit = mesh_bounds.find(platform::ascii_lower(w.mesh_name));
                    if (bit != mesh_bounds.end()) {
                        b = bit->second;
                    }
                    w.pos = {ov.position.x + b.min_x * scale_x, ov.position.y + b.min_y * scale_y};
                    w.size = {(b.max_x - b.min_x) * scale_x, (b.max_y - b.min_y) * scale_y};
                }
            } else {
                // Plain text and the not-yet-modelled sliders / progress bars / combo and text
                // boxes: draw their text only (an empty one draws nothing).
                w.kind = UiWidgetKind::Label;
            }
            if (text_button || mesh_button) {
                w.action_tag = static_cast<std::int32_t>(UiActionKind::OverlayClicked);
                w.action_param = static_cast<std::int32_t>(i);
            }
            const auto ext_it = world.m6.ui.overlay_ext.find(static_cast<std::int32_t>(i));
            if (ext_it != world.m6.ui.overlay_ext.end()) {
                w.enabled = ext_it->second.enabled;
            }
            root.children.push_back(std::move(w));
        }
    }

    // Evaluates pointer hover/click and Escape handling against `root`
    // (`rglCreateSimpleButtonWidget` `0x14031f110`), firing presentation
    // triggers `-63` (`mouse_enter_leave`: param 1 = overlay, param 2 = 0 enter / 1 leave)
    // and `-62` (`state_change`: param 1 = overlay, param 2 = value) and closing
    // `mbNotesWindow` on Return/Escape cleanly (`0x140161ad2..0x140161bd4`). Escape does not
    // close a presentation (`mbPresentationWindow::frameMove` `0x140174ed0` has no key check;
    // see `kPresentationFlagManualEndOnly`): the module's own `-61` trigger handles it.
    std::vector<UiEvent> handle_pointer_and_keys(WseWorldState& world,
                                                 float ui_x,
                                                 float ui_y,
                                                 bool left_down,
                                                 bool left_clicked,
                                                 bool escape_pressed,
                                                 vm::VmContext* vm_ctx = nullptr,
                                                 std::span<const LoadedPresentation> presentations = {}) {
        std::vector<UiEvent> events;
        world.m5.cursor_pos = {ui_x, ui_y};

        if (escape_pressed && current_window == kScreenWindowNotes) {
            const std::string id = root.id;
            close_active_modal_window(world);
            events.push_back(UiEvent{UiActionKind::NotesReturn, -1, id});
            return events;
        }

        const bool can_fire = vm_ctx != nullptr && current_window == kScreenWindowPresentation &&
                              presentation_no_ >= 0;

        // Top-most hit test (iterate children back-to-front in reverse order).
        UiWidget* hovered = find_hit_widget(root, ui_x, ui_y, 0.0f, 0.0f);
        std::vector<std::pair<std::int32_t, bool>> hover_changes;
        update_hover_state(root, hovered, left_down, hover_changes);
        // Copy what the click needs before any trigger can rebuild `root`.
        const std::int32_t hovered_action = (hovered != nullptr) ? hovered->action_tag : -1;
        const std::int32_t hovered_param = (hovered != nullptr) ? hovered->action_param : -1;
        const std::int32_t hovered_overlay = (hovered != nullptr) ? hovered->overlay_id : -1;
        const std::string hovered_id = (hovered != nullptr) ? hovered->id : std::string{};
        const std::int32_t hovered_val =
            (hovered_overlay >= 0 && static_cast<std::size_t>(hovered_overlay) < world.overlays.size())
                ? world.overlays[static_cast<std::size_t>(hovered_overlay)].val
                : 0;

        if (can_fire) {
            for (const auto& [overlay_id, entered] : hover_changes) {
                if (current_window != kScreenWindowPresentation) {
                    break;
                }
                const std::array<std::int64_t, 2> params{overlay_id, entered ? 0 : 1};
                dispatch_presentation_trigger(world, *vm_ctx, -63, params);
                if (!follow_presentation_state(world, *vm_ctx, presentations)) {
                    break;
                }
            }
        }

        if (left_clicked && hovered_action >= 0) {
            const auto action = static_cast<UiActionKind>(hovered_action);
            events.push_back(UiEvent{action, hovered_param, hovered_id});
            if (action == UiActionKind::OverlayClicked && hovered_overlay >= 0 && vm_ctx != nullptr &&
                current_window == kScreenWindowPresentation && presentation_no_ >= 0) {
                const std::array<std::int64_t, 2> params{hovered_overlay, hovered_val};
                dispatch_presentation_trigger(world, *vm_ctx, -62, params);
                (void)follow_presentation_state(world, *vm_ctx, presentations);
            } else if (action == UiActionKind::NotesReturn) {
                close_active_modal_window(world);
            }
        }
        return events;
    }

private:
    std::int32_t presentation_no_ = -1;
    float default_duration_s_ = 100.0f;
    float duration_cs_accum_ = 0.0f;
    // `world.m6.ui.presentation_start_serial` as of the last handled start request.
    std::uint32_t seen_start_serial_ = 0U;
    // Mutable copy of the open presentation's triggers, taken once per presentation
    // instead of once per dispatch.
    SimpleTriggerManager active_triggers_{};

    void reset_root(std::string_view id) {
        root = UiWidget{};
        root.id = std::string(id);
        root.kind = UiWidgetKind::Container;
        root.pos = {0.0f, 0.0f};
        root.size = {kUiScreenWidth, kUiScreenHeight};
    }

    [[nodiscard]] std::int32_t resolve_prev_window(std::int32_t prev_window) const noexcept {
        if (prev_window != kUiReturnToCurrentWindow) {
            return prev_window;
        }
        // A modal window opened from another modal window returns where that one would.
        if (current_window == kScreenWindowNotes || current_window == kScreenWindowPresentation) {
            return source_window;
        }
        return current_window;
    }

    [[nodiscard]] static std::string format_text(std::string_view text, vm::VmContext& ctx) {
        if (text.find('{') == std::string_view::npos && text.find('^') == std::string_view::npos) {
            return std::string(text);
        }
        return core::wse_utf16_to_utf8(vm::wse_parse_string(
            core::wse_utf8_to_utf16(text), ctx.registers, vm::native_string_banks(ctx)));
    }

    void begin_presentation(WseWorldState& world,
                            vm::VmContext& vm_ctx,
                            std::int32_t presentation_no,
                            std::span<const LoadedPresentation> presentations) {
        const LoadedPresentation& prsnt =
            presentations[static_cast<std::size_t>(presentation_no)];
        current_window = kScreenWindowPresentation;
        world.active_presentation_no = presentation_no;
        world.m5.active_window = kScreenWindowPresentation;
        world.m6.ui.active_window_id = kScreenWindowPresentation;
        presentation_no_ = presentation_no;
        active_triggers_ = prsnt.triggers;
        presentation_elapsed_s = 0.0f;
        duration_cs_accum_ = 0.0f;
        // Overlay ids restart at 0 for every presentation.
        world.overlays.clear();
        world.m6.ui.overlay_ext.clear();
        world.m5.current_container_overlay_no = -1;
        // Read-only (flag bit 0) presentations default to 10.0 s (0x1402098c7).
        const float default_s = ((prsnt.flags & 1U) != 0U) ? 10.0f : default_duration_s_;
        const float default_cs = std::clamp(default_s * 100.0f, 0.0f, 2.0e9f);
        world.presentation_duration = static_cast<std::int32_t>(default_cs);
        // Any `start_presentation` issued from here on (including by `-60` itself) is a new
        // request for `follow_presentation_state`.
        seen_start_serial_ = world.m6.ui.presentation_start_serial;
        reset_root("presentation");

        // Fire trigger -60 (`ti_on_presentation_load`, 0x1402099c0..0x140209a60).
        dispatch_presentation_trigger(world, vm_ctx, -60, {});
        sync_overlays_from_world(world);
    }

    // After any presentation trigger: follows a `start_presentation` (900) issued by the
    // trigger, either to a different presentation or to the open one (the Native refresh
    // idiom `presentation_set_duration 0` + `start_presentation <self>`, used by e.g.
    // `prsnt_game_custom_battle_designer` and `prsnt_multiplayer_item_select`, which restarts
    // it: overlays cleared, duration re-armed, `-60` fired again). Bounded, so presentations
    // starting each other (or themselves) from `-60` cannot loop forever. Then closes window
    // `0x16` when the duration ran out, and otherwise re-syncs the overlays. Returns whether a
    // presentation is still open.
    bool follow_presentation_state(WseWorldState& world,
                                   vm::VmContext& vm_ctx,
                                   std::span<const LoadedPresentation> presentations) {
        for (int hops = 0; hops < 8; ++hops) {
            if (current_window != kScreenWindowPresentation) {
                return false;
            }
            const std::int32_t want = world.active_presentation_no;
            const bool requested = world.m6.ui.presentation_start_serial != seen_start_serial_;
            if ((want == presentation_no_ && !requested) || want < 0 ||
                static_cast<std::size_t>(want) >= presentations.size()) {
                break;
            }
            // Consume the window-queue entry `start_presentation` left for this switch.
            for (std::size_t k = world.m5.enqueued_windows.size(); k > 0U; --k) {
                const auto& entry = world.m5.enqueued_windows[k - 1U];
                if (entry.first == kQueuedWindowPresentationType && entry.second == want) {
                    world.m5.enqueued_windows.erase(world.m5.enqueued_windows.begin() +
                                                    static_cast<std::ptrdiff_t>(k - 1U));
                    break;
                }
            }
            begin_presentation(world, vm_ctx, want, presentations);
        }
        // A request still pending after the hop bound is dropped, not replayed next frame.
        seen_start_serial_ = world.m6.ui.presentation_start_serial;
        if (world.active_presentation_no < 0 || world.presentation_duration <= 0) {
            close_active_modal_window(world);
            return false;
        }
        sync_overlays_from_world(world);
        return true;
    }

    // `alpha` replaces the alpha byte of `0xRRGGBB` overlay colours (`overlay_set_alpha` 922
    // stores `alpha / 255`); it does not multiply an alpha the colour may carry.
    [[nodiscard]] static std::uint32_t compose_argb(std::uint32_t rgb, float alpha) noexcept {
        const float a = std::isfinite(alpha) ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
        const auto a8 = static_cast<std::uint32_t>(std::lround(a * 255.0f));
        return (a8 << 24U) | (rgb & 0x00FFFFFFU);
    }

    [[nodiscard]] static std::string u16_to_utf8(std::u16string_view s) {
        std::string out;
        out.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            std::uint32_t cp = s[i];
            if (cp >= 0xD800U && cp <= 0xDBFFU && i + 1U < s.size()) {
                const std::uint32_t lo = s[i + 1U];
                if (lo >= 0xDC00U && lo <= 0xDFFFU) {
                    cp = 0x10000U + (((cp - 0xD800U) << 10U) | (lo - 0xDC00U));
                    ++i;
                }
            }
            if (cp >= 0xD800U && cp <= 0xDFFFU) {
                cp = 0xFFFDU;  // unpaired surrogate
            }
            if (cp < 0x80U) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800U) {
                out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            } else if (cp < 0x10000U) {
                out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            } else {
                out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
            }
        }
        return out;
    }

    // Runs `event_key` of the open presentation with `params` in the trigger-parameter slots
    // read by `store_trigger_param_N` (2070..2073), restoring the previous values afterwards.
    void dispatch_presentation_trigger(WseWorldState& world,
                                       vm::VmContext& vm_ctx,
                                       std::int32_t event_key,
                                       std::span<const std::int64_t> params) {
        WseWorldState& vm_world = vm_ctx.world;
        if (&world != &vm_world) {
            vm_world.active_presentation_no = world.active_presentation_no;
            vm_world.presentation_duration = world.presentation_duration;
            vm_world.overlays = world.overlays;
            vm_world.m6.ui = world.m6.ui;
            vm_world.m5.enqueued_windows = world.m5.enqueued_windows;
        }
        auto& slots = vm_world.misc.trigger_params;
        const auto saved = slots;
        slots.fill(0);
        for (std::size_t k = 0; k < params.size() && k < slots.size(); ++k) {
            slots[k] = params[k];
        }
        (void)vm::execute_simple_trigger_manager(active_triggers_, event_key, vm_ctx.timersTotal,
                                                 vm_ctx);
        slots = saved;
        if (&world != &vm_world) {
            world.active_presentation_no = vm_world.active_presentation_no;
            world.presentation_duration = vm_world.presentation_duration;
            world.overlays = vm_world.overlays;
            world.m6.ui = vm_world.m6.ui;
            world.m5.enqueued_windows = vm_world.m5.enqueued_windows;
        }
    }

    static UiWidget* find_hit_widget(UiWidget& node,
                                     float ui_x,
                                     float ui_y,
                                     float px,
                                     float py) noexcept {
        if (!node.visible) {
            return nullptr;
        }
        const float ax = px + node.pos.x;
        const float ay = py + node.pos.y;
        for (std::size_t k = node.children.size(); k > 0U; --k) {
            if (UiWidget* child_hit =
                    find_hit_widget(node.children[k - 1U], ui_x, ui_y, ax, ay)) {
                return child_hit;
            }
        }
        if ((node.kind == UiWidgetKind::SimpleButton || node.kind == UiWidgetKind::GameButton) &&
            node.hit_test(ui_x, ui_y, px, py)) {
            return &node;
        }
        return nullptr;
    }

    // Updates highlight / pressed flags and records overlay enter (true) / leave (false)
    // transitions; the triggers run afterwards so they cannot rebuild `root` mid-walk.
    static void update_hover_state(UiWidget& node,
                                   const UiWidget* target,
                                   bool left_down,
                                   std::vector<std::pair<std::int32_t, bool>>& changes) {
        const bool was_hl = node.highlighted;
        const bool now_hl = (&node == target);
        node.highlighted = now_hl;
        node.pressed = now_hl && left_down;
        if (was_hl != now_hl && node.overlay_id >= 0) {
            changes.emplace_back(node.overlay_id, now_hl);
        }
        for (UiWidget& c : node.children) {
            update_hover_state(c, target, left_down, changes);
        }
    }
};

}  // namespace wse::game
