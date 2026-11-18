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
#include <map>
#include <optional>
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
    // Multiply the widget's mesh by `color_argb` (mesh overlays and image buttons: retail
    // `rglImageWidget::setColor` / `setAlpha` `0x1402a6c50` / `0x1402a6c90` and
    // `rglImageButtonWidget::setColor` `0x1402a6a80` write it into the widget mesh's colour;
    // game buttons keep `rglWidget::setColor` `0x14031e890`, which leaves their meshes alone).
    bool tint_mesh = false;
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
    // Container overlays (`set_container_overlay` 945): children are placed at
    // `pos + child_offset` (the scroll position of a scrollable container) and, when
    // `clip_children` is set, are neither hit outside this widget's box nor drawn outside it
    // (scissor-clipped to the container's box; fully outside children may still be skipped).
    wse::core::WseVec2 child_offset{};
    bool clip_children = false;
    // Draw the mesh into this rectangle (relative to `pos`) instead of stretching it over the
    // widget box: game buttons keep their native-scaled mesh while the box is the hit area.
    bool has_mesh_rect = false;
    wse::core::WseVec2 mesh_pos{};
    wse::core::WseVec2 mesh_size{};
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
// Game-button meshes are not stretched over the box. `createGameButtonOverlay` (`0x14020c3e0`)
// scales the `medium_button` meshes uniformly by `0x3f0e38e4` (5/9) and stores the scale
// `(0.148, 0.04)`; `createIngameButtonOverlay` (`0x14020c7d0`) keeps `mp_inventory_button` at its
// native size and stores the scale `(5.3075, 1.0)` (`0x40a9d70a`). `rglGameButtonWidget::setScale`
// (`0x14029bfb0`) multiplies the meshes by `new / stored` per axis.
inline constexpr float kUiGameButtonMeshScale = 5.0f / 9.0f;
inline constexpr float kUiInGameButtonStoredScaleX = 5.3075f;
inline constexpr float kUiInGameButtonStoredScaleY = 1.0f;

// --- Interactive overlays ---------------------------------------------------------------
// Retail input path. `rglWindowManager::frameMove` (`0x14032a0d0`) sends mouse presses as
// widget states `0x10` (left) / `0x40` (right) and releases as `0x8` / `0x20`, to the focused
// widget if there is one, otherwise to the window's widget tree. `rglContainerWidget::triggerState`
// (`0x14028af30`) sends hover enter `0x2` / leave `0x4` to every visible, enabled child the
// cursor enters or leaves, whatever its type, and hands a press to the children under the
// cursor from the top down until one consumes it. `rglWidget::triggerState` (`0x14031dd10`)
// latches each state with `rglWidget::setState` (`0x14031d120`), and
// `mbPresentationContainer::frameMove` (`0x140208ec0`) reads the latched states back per
// overlay, in overlay order, with `rglWidget::checkState` (`0x14031d100`):
// `0x2` -> `-63` (param 2 = 0), `0x4` -> `-63` (1), `0x10000` (value changed) -> `-62`,
// `0x10` / `0x40` / `0x100` -> `-64` (param 2 = 0 / 1 / 2).
inline constexpr std::uint32_t kUiStateEnter = 0x2U;
inline constexpr std::uint32_t kUiStateLeave = 0x4U;
inline constexpr std::uint32_t kUiStateLeftPress = 0x10U;
inline constexpr std::uint32_t kUiStateRightPress = 0x40U;
inline constexpr std::uint32_t kUiStateMiddlePress = 0x100U;
inline constexpr std::uint32_t kUiStateChanged = 0x10000U;
// `create_slider_overlay` (914, `createSliderOverlay` `0x14020af30`): a horizontal
// `rglSliderWidget` 0.25 wide (`slider_hor` / `handle_hor` meshes) with a 0.008 knob margin
// (`0x3c03126f`). `rglSliderWidget::triggerState` (`0x14030d610`) keeps the value when a press
// lands within `margin + 0.01` of the knob and otherwise jumps to the cursor
// (`rglSliderWidget::setAbsolutePosition` `0x14030da60`); while dragging, every cursor move
// sets the value again and raises `0x10000`. `-62` reports the value truncated toward zero.
// The height comes from the `slider_hor` mesh; without its bounds the port uses the fallback.
inline constexpr float kUiSliderWidth = 0.25f;
inline constexpr float kUiSliderMargin = 0.008f;
inline constexpr float kUiSliderKnobTolerance = 0.01f;
inline constexpr float kUiSliderFallbackHeight = 0.03f;
// `rglNumberBoxWidget::triggerState` (`0x1402c8310`): a press on an arrow steps the value by
// 1 at once; while the arrow is held the value repeats after 0.5 s every 0.1 s, by 1 up to
// 2.5 s held, then 3 up to 5 s, then 10 up to 7.5 s, then 50, clamped to the bounds.
// Releasing or leaving the box stops it.
inline constexpr float kUiNumberBoxRepeatDelay = 0.5f;
inline constexpr float kUiNumberBoxRepeatInterval = 0.1f;
// Port approximations (layouts not traced): number boxes, combo buttons / labels and list
// boxes get these boxes, the outer quarter on each side of a number box or combo label is its
// decrement (left) / increment (right) arrow, and combo popups list their items downwards
// from the box (`rglComboButtonWidget::openPopup` `0x140261d10` not traced).
inline constexpr float kUiNumberBoxWidth = 0.12f;
inline constexpr float kUiComboWidth = 0.25f;
inline constexpr float kUiInputBoxHeight = 0.04f;
inline constexpr float kUiArrowFraction = 0.25f;
// `create_text_overlay` (910, `createTextOverlay` `0x140209a90`) builds an
// `rglScrollableWidget` (`rglScrollableWidget::getType` `0x140300270` = 8) when text flag bit
// 17, 18 or 19 is set; `set_container_overlay` children then live inside it. The wheel scrolls
// the scrollable container nearest the cursor (`mbPresentationContainer::frameMove`). The step
// per wheel notch and the initial position (content top at the area top) are port choices.
inline constexpr std::uint32_t kUiTextFlagsScrollable = 0x000E0000U;
// `rglTextBoxWidget` constructor (`0x140316900`): `+0x294` caps a text box at 28 characters
// (`rglString::getUnicodeLength`, so code points, not bytes).
inline constexpr std::size_t kUiTextBoxMaxLength = 28U;
inline constexpr float kUiScrollStep = 0.05f;
inline constexpr float kUiTouchDragScrollThreshold = 0.01f;

// One frame of pointer input for `UiSession::handle_input`. `*_pressed` / `left_released`
// are edges seen this frame; `wheel` is in notches (positive = away from the user);
// `time_s` is a monotonic clock used for number-box auto-repeat; `touch` is true when the
// pointer is a finger this frame (enabling touch drag-to-scroll without changing mouse behaviour).
struct UiPointerInput {
    float x = 0.0f;
    float y = 0.0f;
    bool left_down = false;
    bool left_pressed = false;
    bool left_released = false;
    bool right_pressed = false;
    bool middle_pressed = false;
    bool escape_pressed = false;
    bool touch = false;
    float wheel = 0.0f;
    float time_s = 0.0f;
    // Characters typed this frame, in order (`rglWindowManager` delivers each as state `0x1000`
    // with the code in `+0x3498`): printable text, `\b` (8) backspace, `\r` (13) Enter,
    // `\x1b` (27) Escape.
    std::u16string text_input{};
};

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

// Input behaviour of a presentation overlay, after its retail widget class
// (`rglWidget::getType`, read by `mbPresentationContainer::frameMove` `0x140208ec0` to pick
// the `-62` value: sliders / progress bars report the float value, number boxes / combo
// buttons / combo labels / list boxes the int value, check boxes their toggle state).
enum class UiOverlayInput : std::uint8_t {
    None = 0,         // text, mesh, progress bar: hover and press only
    Button = 1,       // 912 / 940 / 941 text buttons, 913 / 938 image buttons
    CheckBox = 2,     // 918 (`createCheckBoxOverlay` `0x14020bc50`: image button, toggle flag)
    Slider = 3,       // 914
    NumberBox = 4,    // 942
    ComboButton = 5,  // 916
    ComboLabel = 6,   // 948
    ListBox = 7,      // 943 (port: behaves as a combo button)
    TextBox = 8,      // 917 / 919 (text entry not modelled yet)
};

[[nodiscard]] inline UiOverlayInput overlay_input_kind(const WsePresentationOverlay& ov) noexcept {
    switch (ov.m5.create_opcode) {
    case 914:
        return UiOverlayInput::Slider;
    case 942:
        return UiOverlayInput::NumberBox;
    case 916:
        return UiOverlayInput::ComboButton;
    case 948:
        return UiOverlayInput::ComboLabel;
    case 943:
        return UiOverlayInput::ListBox;
    case 917:
    case 919:
        return UiOverlayInput::TextBox;
    default:
        break;
    }
    if (ov.type == 1 && ov.m5.is_button) {
        return UiOverlayInput::Button;
    }
    if (ov.type == 4) {
        return ov.m5.is_checkbox ? UiOverlayInput::CheckBox : UiOverlayInput::Button;
    }
    // Overlays built without a create opcode (hosts, tests): fall back on the port type.
    if (ov.m5.create_opcode == 0) {
        switch (ov.type) {
        case 2:
            return UiOverlayInput::Slider;
        case 5:
            return UiOverlayInput::ComboButton;
        case 8:
            return UiOverlayInput::ListBox;
        default:
            break;
        }
    }
    return UiOverlayInput::None;
}

// Axis-aligned UI-space box (`x0 <= x1`, `y0 <= y1`).
struct UiRect {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    [[nodiscard]] bool contains(float x, float y) const noexcept {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1;
    }
    [[nodiscard]] UiRect intersect(const UiRect& o) const noexcept {
        return UiRect{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
    }
};

// An overlay's absolute box and the clip rectangle of its containers.
struct UiHitEntry {
    std::int32_t overlay_id = -1;
    UiRect box{};
    UiRect clip{};
    bool scrollable = false;

    [[nodiscard]] bool contains(float x, float y) const noexcept {
        return box.contains(x, y) && clip.contains(x, y);
    }
    [[nodiscard]] bool on_left_arrow(float x) const noexcept {
        return x <= box.x0 + (box.x1 - box.x0) * kUiArrowFraction;
    }
    [[nodiscard]] bool on_right_arrow(float x) const noexcept {
        return x >= box.x1 - (box.x1 - box.x0) * kUiArrowFraction;
    }
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
            if (world.m5.focused_overlay_no >= 0 &&
                static_cast<std::size_t>(world.m5.focused_overlay_no) < world.overlays.size()) {
                world.overlays[static_cast<std::size_t>(world.m5.focused_overlay_no)].m5.focused =
                    false;
            }
            world.m5.focused_overlay_no = -1;
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
        reset_input_state();
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
        animate_overlays(world, dt);
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
        bool opened_menu = false;          // menu window 0x12 built this frame
        std::size_t mission_presentations_open = 0;  // conversation-window containers
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
    //   a campaign window back. The conversation window's containers
    //   (`start_conversation_presentation` 4907) run in `update_mission_presentations`, and
    //   while any is open `root` draws them (`compose_mission_root`).
    // - a consumed `0x12` (`jump_to_menu` 2060, `mbMapWindow::frameMove` `0x140117f90` opens
    //   `mbMenuWindow` `0x12`) builds the menu window for `world.jump_menu_no` from `menus`
    //   (`mbMenuWindow::open` `0x140120240`, which runs the menu's operations), closing an open
    //   notes / presentation window first. Port: the menu number comes from `jump_to_menu`,
    //   and `world.ui.current_menu_no` records it. Without `menus` the queue is still consumed
    //   but no menu is built.
    FrameReport drive_frame(WseWorldState& world,
                            vm::VmContext& vm_ctx,
                            float dt_s,
                            std::span<const LoadedPresentation> presentations,
                            std::int32_t window_before_frame,
                            std::span<const LoadedMenu> menus = {}) {
        FrameReport rep{};
        const bool modal_open =
            current_window == kScreenWindowNotes || current_window == kScreenWindowPresentation;
        if (world.lighting.has_scene) {
            update_mission_presentations(world, vm_ctx, dt_s, presentations);
            rep.mission_presentations_open = mission_stack_.size();
            if (modal_open) {
                const std::int32_t m5_win = world.m5.active_window;
                const std::int32_t ui_win = world.m6.ui.active_window_id;
                close_active_modal_window(world);
                world.m5.active_window = m5_win;
                world.m6.ui.active_window_id = ui_win;
                current_window = -1;
                rep.closed_for_mission = true;
            }
            if (!mission_stack_.empty() || root.id == "mission") {
                compose_mission_root();
            }
            return rep;
        }

        if (!mission_stack_.empty()) {
            // Leaving the scene closes the conversation window and its containers.
            mission_stack_.clear();
            world.m6.ui.active_presentations.clear();
        }
        if (root.id == "mission") {
            reset_root("closed");
        }
        world.m6.ui.pending_conversation_presentations.clear();
        world.m6.ui.mission_duration_requests.clear();
        const std::int32_t win = world.m5.active_window;
        if (win == kScreenWindowMenu && world.m6.ui.menu_open_serial != seen_menu_serial_) {
            seen_menu_serial_ = world.m6.ui.menu_open_serial;
            const std::int32_t menu_no = world.jump_menu_no;
            if (menu_no >= 0 && static_cast<std::size_t>(menu_no) < menus.size()) {
                if (modal_open) {
                    close_active_modal_window(world);
                }
                world.ui.current_menu_no = menu_no;
                build_menu_window(world, menu_no, menus, &vm_ctx);
                rep.opened_menu = true;
            }
            rep.presentation_open = current_window == kScreenWindowPresentation;
            return rep;
        }
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
        const auto remember = [&prev_state](const auto& self, const UiWidget& node) -> void {
            for (const UiWidget& old_w : node.children) {
                if (old_w.overlay_id >= 0) {
                    prev_state[old_w.overlay_id] = {old_w.highlighted, old_w.pressed};
                }
                self(self, old_w);
            }
        };
        remember(remember, root);
        reset_root("presentation");

        const std::size_t n_ov = world.overlays.size();
        std::vector<UiWidget> built(n_ov);
        std::vector<bool> present(n_ov, false);
        for (std::size_t i = 0; i < n_ov; ++i) {
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
                    const bool game = (op == 940);
                    const float base = game ? kUiGameButtonMeshScale : 1.0f;
                    float kx = base;
                    float ky = base;
                    if (ov.m5.size_set) {
                        kx *= scale_x / (game ? kUiGameButtonWidth : kUiInGameButtonStoredScaleX);
                        ky *= scale_y / (game ? kUiGameButtonHeight : kUiInGameButtonStoredScaleY);
                    }
                    UiMeshBounds b{};
                    if (const auto bit = mesh_bounds.find(w.mesh_name); bit != mesh_bounds.end()) {
                        b = bit->second;
                    }
                    w.has_mesh_rect = true;
                    w.mesh_pos = {b.min_x * kx, b.min_y * ky};
                    w.mesh_size = {(b.max_x - b.min_x) * kx, (b.max_y - b.min_y) * ky};
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
                        // Box = text extent at the text size in force when the text was last
                        // set (see `kUiOverlayButtonWidth` and `m5.text_measure_scale`).
                        const auto ms = ov.m5.text_measure_scale;
                        const float msx = std::isfinite(ms.x) ? std::max(0.0f, ms.x) : 1.0f;
                        const float msy = std::isfinite(ms.y) ? std::max(0.0f, ms.y) : 1.0f;
                        const auto [tw, th] = font->measure_text(
                            w.text, 0.0f, kUiOverlayTextScale * msx, kUiOverlayTextScale * msy,
                            w.text_flags);
                        w.size = {tw, th};
                    } else {
                        w.size = {kUiOverlayButtonWidth * scale_x, kUiOverlayButtonHeight * scale_y};
                    }
                }
            } else if (mesh_button || (ov.type == 0 && !w.mesh_name.empty())) {
                w.kind = mesh_button ? UiWidgetKind::GameButton : UiWidgetKind::Image;
                w.tint_mesh = true;
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
                // Text, progress bars, text boxes, and the input widgets below draw their text
                // only (an empty one draws nothing); the input widgets also get a hit box.
                w.kind = UiWidgetKind::Label;
                const UiOverlayInput input_kind = overlay_input_kind(ov);
                const auto value_text = [&ov]() {
                    if (ov.val >= 0 && static_cast<std::size_t>(ov.val) < ov.m5.items.size()) {
                        return u16_to_utf8(ov.m5.items[static_cast<std::size_t>(ov.val)]);
                    }
                    return std::string{};
                };
                switch (input_kind) {
                case UiOverlayInput::Slider: {
                    float h = kUiSliderFallbackHeight;
                    if (const auto bit = mesh_bounds.find("slider_hor"); bit != mesh_bounds.end()) {
                        h = bit->second.max_y - bit->second.min_y;
                    }
                    if (!has_area) {
                        w.size = {kUiSliderWidth, h};
                    }
                    // `createSliderOverlay` (`0x14020af30`) sets the `slider_hor` track mesh
                    // and the `handle_hor` knob; the knob sits at the value's position
                    // between the margins (`rglSliderWidget::getPercentage` `0x14030dae0`).
                    w.kind = UiWidgetKind::Image;
                    w.mesh_name = "slider_hor";
                    w.tint_mesh = true;
                    UiWidget knob{};
                    knob.id = w.id + "_knob";
                    knob.kind = UiWidgetKind::Image;
                    knob.mesh_name = "handle_hor";
                    knob.tint_mesh = true;
                    knob.color_argb = w.color_argb;
                    UiMeshBounds kb{0.0f, 0.0f, 0.02f, h};
                    if (const auto kit = mesh_bounds.find("handle_hor"); kit != mesh_bounds.end()) {
                        kb = kit->second;
                    }
                    const double range = ov.m5.max_value - ov.m5.min_value;
                    const float pct =
                        (range != 0.0)
                            ? std::clamp(static_cast<float>((ov.val - ov.m5.min_value) / range), 0.0f, 1.0f)
                            : 0.0f;
                    const float knob_x = kUiSliderMargin + pct * (w.size.x - 2.0f * kUiSliderMargin);
                    knob.pos = {knob_x + kb.min_x, kb.min_y};
                    knob.size = {kb.max_x - kb.min_x, kb.max_y - kb.min_y};
                    w.children.push_back(std::move(knob));
                    break;
                }
                case UiOverlayInput::NumberBox:
                    // While its text box has the focus it shows the text being typed.
                    w.text = ov.m5.focused ? u16_to_utf8(ov.text) : std::to_string(ov.val);
                    if (!has_area) {
                        w.size = {kUiNumberBoxWidth, kUiInputBoxHeight};
                    }
                    break;
                case UiOverlayInput::TextBox:
                    if (!has_area) {
                        w.size = {kUiComboWidth, kUiInputBoxHeight};
                    }
                    break;
                case UiOverlayInput::ComboButton:
                case UiOverlayInput::ComboLabel:
                case UiOverlayInput::ListBox:
                    w.text = value_text();
                    if (!has_area) {
                        w.size = {kUiComboWidth, kUiInputBoxHeight};
                    }
                    break;
                default:
                    break;
                }
                if (ov.type == 1 && !ov.m5.is_button && has_area &&
                    (static_cast<std::uint32_t>(ov.val) & kUiTextFlagsScrollable) != 0U) {
                    w.clip_children = true;
                }
            }
            if (text_button || mesh_button) {
                w.action_tag = static_cast<std::int32_t>(UiActionKind::OverlayClicked);
                w.action_param = static_cast<std::int32_t>(i);
            }
            const auto ext_it = world.m6.ui.overlay_ext.find(static_cast<std::int32_t>(i));
            if (ext_it != world.m6.ui.overlay_ext.end()) {
                w.enabled = ext_it->second.enabled;
            }
            built[i] = std::move(w);
            present[i] = true;
        }

        // Nest `set_container_overlay` children inside their container. A child whose
        // container is hidden is hidden too; a missing container or a container cycle
        // (`overlay_set_container_overlay` 951 can make one) leaves the overlay at the top.
        const auto container_of = [&world, n_ov](std::size_t i) -> std::int64_t {
            const std::int32_t c = world.overlays[i].m5.container_overlay_no;
            return (c >= 0 && static_cast<std::size_t>(c) < n_ov && static_cast<std::size_t>(c) != i)
                       ? c
                       : -1;
        };
        std::vector<std::vector<std::size_t>> kids(n_ov);
        std::vector<std::size_t> top;
        for (std::size_t i = 0; i < n_ov; ++i) {
            if (!present[i]) {
                continue;
            }
            const std::int64_t c = container_of(i);
            bool cycle = false;
            for (std::int64_t at = c, hops = 0; at >= 0; at = container_of(static_cast<std::size_t>(at)), ++hops) {
                if (static_cast<std::size_t>(at) == i || static_cast<std::size_t>(hops) > n_ov) {
                    cycle = true;
                    break;
                }
            }
            if (c < 0 || cycle) {
                top.push_back(i);
            } else if (present[static_cast<std::size_t>(c)]) {
                kids[static_cast<std::size_t>(c)].push_back(i);
            }
        }
        const auto attach = [&](const auto& self, std::size_t i) -> UiWidget {
            UiWidget w = std::move(built[i]);
            for (const std::size_t k : kids[i]) {
                w.children.push_back(self(self, k));
            }
            if (w.clip_children) {
                // Scrollable container: content top starts at the area top; the scroll
                // position moves it up by up to (content height - area height).
                float content_top = 0.0f;
                for (const UiWidget& c : w.children) {
                    content_top = std::max(content_top, c.pos.y + c.size.y);
                }
                const float max_scroll = std::max(0.0f, content_top - w.size.y);
                float& scroll = scroll_pos_[static_cast<std::int32_t>(i)];
                scroll = std::clamp(scroll, 0.0f, max_scroll);
                w.child_offset = {0.0f, (w.size.y - content_top) + scroll};
            }
            return w;
        };
        for (const std::size_t t : top) {
            root.children.push_back(attach(attach, t));
        }
        append_combo_popup(world);
    }

    // Pointer and key input for the open window (`UiPointerInput`, one call per frame).
    //
    // Built-in windows: buttons highlight under the cursor and act on a left release over the
    // button the left press landed on (`rglSimpleButtonWidget::triggerState` `0x140307770`);
    // Return/Escape closes `mbNotesWindow` cleanly (`0x140161ad2..0x140161bd4`).
    //
    // Presentations follow the retail state path described at `kUiStateEnter`:
    // - every visible, enabled overlay whose box holds the cursor gets enter / leave (`-63`),
    //   whatever its type (text and mesh overlays included);
    // - a press goes to the overlays under the cursor from the top down until one consumes it,
    //   each of them getting `-64`; buttons, check boxes, sliders, combo buttons and list
    //   boxes consume a left press, number boxes and combo labels only on their arrows;
    // - text / image / game buttons arm on the left press, disarm when the cursor leaves, and
    //   fire `-62` on a left release over them (param 2 = the overlay value; retail leaves a
    //   stale trigger parameter there, see `engine/README.md`); a check box toggles on the
    //   press and reports the new state; sliders, number boxes, combo labels and combo popups
    //   change their value and report it;
    // - the wheel scrolls the scrollable container nearest the cursor.
    // The triggers then run per overlay in overlay order (enter, leave, change, left / right /
    // middle press), stopping when one closes or restarts the presentation. Escape does not
    // close a presentation (`mbPresentationWindow::frameMove` `0x140174ed0` has no key check;
    // see `kPresentationFlagManualEndOnly`): the module's own `-61` trigger handles it.
    // Port: the triggers run here, at input time, rather than after `-61` in the frame.
    std::vector<UiEvent> handle_input(WseWorldState& world,
                                      const UiPointerInput& in,
                                      vm::VmContext* vm_ctx = nullptr,
                                      std::span<const LoadedPresentation> presentations = {}) {
        world.m5.cursor_pos = {in.x, in.y};
        if (!in_mission_container_ && !mission_stack_.empty() &&
            current_window != kScreenWindowPresentation && current_window != kScreenWindowNotes) {
            return handle_mission_input(world, in, vm_ctx, presentations);
        }
        std::vector<UiEvent> events;

        if (in.escape_pressed && current_window == kScreenWindowNotes) {
            const std::string id = root.id;
            close_active_modal_window(world);
            events.push_back(UiEvent{UiActionKind::NotesReturn, -1, id});
            return events;
        }

        // Button highlight / pressed visuals (top-most button under the cursor).
        UiWidget* hovered = find_hit_widget(root, in.x, in.y, 0.0f, 0.0f);
        update_hover_state(root, hovered, in.left_down);

        if (current_window != kScreenWindowPresentation || presentation_no_ < 0) {
            const std::string hovered_id = (hovered != nullptr) ? hovered->id : std::string{};
            if (in.left_pressed) {
                armed_widget_id_ = hovered_id;
            }
            if (in.left_released) {
                const bool fire = hovered != nullptr && hovered->action_tag >= 0 &&
                                  !armed_widget_id_.empty() && armed_widget_id_ == hovered_id;
                armed_widget_id_.clear();
                if (fire) {
                    const auto action = static_cast<UiActionKind>(hovered->action_tag);
                    events.push_back(UiEvent{action, hovered->action_param, hovered_id});
                    if (action == UiActionKind::NotesReturn) {
                        close_active_modal_window(world);
                    }
                }
            }
            return events;
        }

        std::vector<UiHitEntry> hits;
        collect_overlay_hits(root, 0.0f, 0.0f, UiRect{-1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f}, hits);
        std::map<std::int32_t, std::uint32_t> states;
        const auto raise = [&states](std::int32_t id, std::uint32_t bits) { states[id] |= bits; };
        const auto valid = [&world](std::int32_t id) {
            return id >= 0 && static_cast<std::size_t>(id) < world.overlays.size();
        };

        // Hover enter / leave.
        std::vector<std::int32_t> inside;
        for (const UiHitEntry& e : hits) {
            if (e.contains(in.x, in.y)) {
                inside.push_back(e.overlay_id);
            }
        }
        std::sort(inside.begin(), inside.end());
        inside.erase(std::unique(inside.begin(), inside.end()), inside.end());
        for (const std::int32_t id : inside) {
            if (!std::binary_search(hovered_overlays_.begin(), hovered_overlays_.end(), id)) {
                raise(id, kUiStateEnter);
            }
        }
        for (const std::int32_t id : hovered_overlays_) {
            if (!std::binary_search(inside.begin(), inside.end(), id)) {
                raise(id, kUiStateLeave);
                if (id == armed_overlay_) {
                    armed_overlay_ = -1;
                }
                if (id == hold_overlay_) {
                    hold_overlay_ = -1;
                }
            }
        }
        hovered_overlays_ = std::move(inside);

        for (std::size_t i = 0; i < world.overlays.size(); ++i) {
            if (world.overlays[i].m5.commit_pending) {
                world.overlays[i].m5.commit_pending = false;
                raise(static_cast<std::int32_t>(i), kUiStateChanged);
            }
        }
        const std::int32_t text_focus = focused_text_box(world);
        if (text_focus >= 0) {
            // A focused text box has the window manager's focus: it gets every press and the
            // typed characters instead of the tree (`rglTextBoxWidget::triggerState`
            // `0x140316bd0`). Enter / Escape, or a left press outside the box, commit the text
            // (`rglTextBoxWidget::loseFocus` `0x140317220` raises `0x10000`).
            WsePresentationOverlay& tb = world.overlays[static_cast<std::size_t>(text_focus)];
            const bool numeric = overlay_input_kind(tb) == UiOverlayInput::NumberBox;
            bool commit = false;
            for (const char16_t c : in.text_input) {
                if (c == u'\r' || c == 0x1BU) {
                    commit = true;
                    break;
                }
                if (numeric) {
                    // Numeric mode (`0x140316ce7`): a digit makes the value
                    // `strtol(text) * 10 + digit`, clamped with a minimum of 0; backspace
                    // erases the last character; anything else is ignored.
                    if (c >= u'0' && c <= u'9') {
                        set_number_box_value(tb, number_box_text_value(tb.text) * 10 + (c - u'0'),
                                             std::int64_t{0});
                    } else if (c == u'\b' && !tb.text.empty()) {
                        tb.text.pop_back();
                    }
                    continue;
                }
                if (c == u'\b') {
                    if (!tb.text.empty()) {
                        const char16_t last = tb.text.back();
                        tb.text.pop_back();
                        if (last >= 0xDC00U && last <= 0xDFFFU && !tb.text.empty() &&
                            tb.text.back() >= 0xD800U && tb.text.back() <= 0xDBFFU) {
                            tb.text.pop_back();
                        }
                    }
                } else if (c >= 0x20U && c != u'<' && c != u'>' &&
                           utf16_code_points(tb.text) < kUiTextBoxMaxLength) {
                    tb.text.push_back(c);
                }
            }
            const UiHitEntry* e = find_hit_entry(hits, text_focus);
            if (in.left_pressed) {
                press_consumed_ = true;
                raise(text_focus, kUiStateLeftPress);
                // A number box's text box sits between its arrows, so a press on an arrow is
                // outside the focused widget too.
                if (e == nullptr || !e->contains(in.x, in.y) ||
                    (numeric && (e->on_left_arrow(in.x) || e->on_right_arrow(in.x)))) {
                    commit = true;
                }
            }
            if (in.right_pressed) {
                raise(text_focus, kUiStateRightPress);
            }
            if (in.middle_pressed) {
                raise(text_focus, kUiStateMiddlePress);
            }
            if (commit) {
                if (numeric) {
                    // `0x1403170db`: the text is read back through `setIntValue`.
                    set_number_box_value(tb, number_box_text_value(tb.text));
                }
                tb.m5.focused = false;
                world.m5.focused_overlay_no = -1;
                raise(text_focus, kUiStateChanged);
            }
        } else if (drag_overlay_ >= 0) {
            // A dragged slider has the focus: it gets the input instead of the tree.
            const UiHitEntry* e = find_hit_entry(hits, drag_overlay_);
            if (!valid(drag_overlay_) || e == nullptr || in.left_released || !in.left_down) {
                drag_overlay_ = -1;
            } else if (in.x != drag_last_x_) {
                drag_last_x_ = in.x;
                set_slider_from_cursor(world.overlays[static_cast<std::size_t>(drag_overlay_)], *e,
                                       in.x - drag_grab_offset_);
                raise(drag_overlay_, kUiStateChanged);
            }
        } else if (popup_overlay_ >= 0 && (in.left_pressed || in.right_pressed)) {
            // An open combo popup takes the next press: an item selects it, anything closes.
            const std::int32_t combo = popup_overlay_;
            popup_overlay_ = -1;
            press_consumed_ = in.left_pressed;
            if (in.left_pressed && valid(combo)) {
                for (std::size_t k = 0; k < popup_items_.size(); ++k) {
                    if (popup_items_[k].contains(in.x, in.y)) {
                        world.overlays[static_cast<std::size_t>(combo)].val =
                            static_cast<std::int32_t>(k);
                        raise(combo, kUiStateChanged);
                        break;
                    }
                }
            }
            popup_items_.clear();
        } else {
            const std::array<std::pair<bool, std::uint32_t>, 3> presses{
                std::pair{in.left_pressed, kUiStateLeftPress},
                std::pair{in.right_pressed, kUiStateRightPress},
                std::pair{in.middle_pressed, kUiStateMiddlePress}};
            for (const auto& [pressed, bit] : presses) {
                if (!pressed) {
                    continue;
                }
                for (std::size_t k = hits.size(); k > 0U; --k) {
                    const UiHitEntry& e = hits[k - 1U];
                    if (!e.contains(in.x, in.y) || !valid(e.overlay_id)) {
                        continue;
                    }
                    raise(e.overlay_id, bit);
                    if (bit == kUiStateLeftPress && left_press(world, e, in, raise)) {
                        press_consumed_ = true;
                        break;
                    }
                }
            }
            if (in.left_pressed) {
                scroll_drag_overlay_ = -1;
                scroll_drag_press_y_ = 0.0f;
                scroll_drag_last_y_ = 0.0f;
                scroll_drag_active_ = false;
                if (in.touch && drag_overlay_ < 0 && popup_overlay_ < 0 &&
                    focused_text_box(world) < 0 && std::isfinite(in.y)) {
                    for (std::size_t k = hits.size(); k > 0U; --k) {
                        const UiHitEntry& e = hits[k - 1U];
                        if (e.scrollable && e.contains(in.x, in.y) && valid(e.overlay_id)) {
                            scroll_drag_overlay_ = e.overlay_id;
                            scroll_drag_press_y_ = in.y;
                            scroll_drag_last_y_ = in.y;
                            break;
                        }
                    }
                }
            } else if (scroll_drag_overlay_ >= 0) {
                if (in.touch && (in.left_down || in.left_released) && valid(scroll_drag_overlay_) &&
                    world.overlays[static_cast<std::size_t>(scroll_drag_overlay_)].visible &&
                    std::isfinite(in.y)) {
                    if (!scroll_drag_active_ &&
                        std::fabs(in.y - scroll_drag_press_y_) > kUiTouchDragScrollThreshold) {
                        scroll_drag_active_ = true;
                        armed_overlay_ = -1;
                        hold_overlay_ = -1;
                        armed_widget_id_.clear();
                        clear_pressed_state(root);
                    }
                    if (scroll_drag_active_) {
                        const float dy = in.y - scroll_drag_last_y_;
                        scroll_drag_last_y_ = in.y;
                        scroll_pos_[scroll_drag_overlay_] =
                            std::max(0.0f, scroll_pos_[scroll_drag_overlay_] + dy);
                    }
                } else {
                    scroll_drag_overlay_ = -1;
                    scroll_drag_press_y_ = 0.0f;
                    scroll_drag_last_y_ = 0.0f;
                    scroll_drag_active_ = false;
                }
            }
        }

        if (in.left_released) {
            if (armed_overlay_ >= 0 && valid(armed_overlay_) &&
                std::binary_search(hovered_overlays_.begin(), hovered_overlays_.end(),
                                   armed_overlay_)) {
                raise(armed_overlay_, kUiStateChanged);
                events.push_back(UiEvent{UiActionKind::OverlayClicked, armed_overlay_,
                                         "overlay_" + std::to_string(armed_overlay_)});
            }
            armed_overlay_ = -1;
            hold_overlay_ = -1;
            scroll_drag_overlay_ = -1;
            scroll_drag_press_y_ = 0.0f;
            scroll_drag_last_y_ = 0.0f;
            scroll_drag_active_ = false;
        } else if (!in.left_down) {
            scroll_drag_overlay_ = -1;
            scroll_drag_press_y_ = 0.0f;
            scroll_drag_last_y_ = 0.0f;
            scroll_drag_active_ = false;
        }

        // Number-box auto-repeat while an arrow stays held.
        if (hold_overlay_ >= 0 && in.left_down && valid(hold_overlay_)) {
            const float held = in.time_s - hold_start_s_;
            if (held > kUiNumberBoxRepeatDelay &&
                held - hold_last_s_ >= kUiNumberBoxRepeatInterval) {
                hold_last_s_ = held;
                const std::int32_t step =
                    held <= 2.5f ? 1 : (held <= 5.0f ? 3 : (held <= 7.5f ? 10 : 50));
                if (const UiHitEntry* e = find_hit_entry(hits, hold_overlay_)) {
                    // Retail re-tests the increment arrow only: anywhere else steps down.
                    step_number_box(world.overlays[static_cast<std::size_t>(hold_overlay_)],
                                    e->on_right_arrow(in.x) ? step : -step);
                    raise(hold_overlay_, kUiStateChanged);
                }
            }
        } else if (!in.left_down) {
            hold_overlay_ = -1;
        }

        if (in.wheel != 0.0f) {
            scroll_nearest_container(hits, in.x, in.y, in.wheel);
        }

        if (vm_ctx != nullptr && !states.empty()) {
            dispatch_overlay_states(world, *vm_ctx, states, presentations);
        }
        if (current_window == kScreenWindowPresentation) {
            sync_overlays_from_world(world);
        }
        return events;
    }

    // Single-button form of `handle_input`: `left_clicked` is a left press and release in the
    // same frame (a tap).
    std::vector<UiEvent> handle_pointer_and_keys(WseWorldState& world,
                                                 float ui_x,
                                                 float ui_y,
                                                 bool left_down,
                                                 bool left_clicked,
                                                 bool escape_pressed,
                                                 vm::VmContext* vm_ctx = nullptr,
                                                 std::span<const LoadedPresentation> presentations = {}) {
        UiPointerInput in{};
        in.x = ui_x;
        in.y = ui_y;
        in.left_down = left_down;
        in.left_pressed = left_clicked;
        in.left_released = left_clicked;
        in.escape_pressed = escape_pressed;
        return handle_input(world, in, vm_ctx, presentations);
    }

    // Overlay id of the open combo popup (-1 when none) and its item boxes.
    [[nodiscard]] std::int32_t combo_popup_overlay() const noexcept { return popup_overlay_; }

    // True when a text or number box overlay currently holds keyboard focus (`world.m5.focused_overlay_no`
    // in a campaign presentation, or a focused overlay inside an active mission presentation).
    [[nodiscard]] bool has_focused_text_input(const WseWorldState& world) const noexcept {
        if (current_window == kScreenWindowPresentation) {
            return focused_text_box(world) >= 0;
        }
        if (!in_mission_container_ && !mission_stack_.empty()) {
            for (const MissionPresentation& mp : mission_stack_) {
                const std::int32_t id = mp.focused_overlay;
                if (id >= 0 && static_cast<std::size_t>(id) < mp.overlays.size()) {
                    const WsePresentationOverlay& ov = mp.overlays[static_cast<std::size_t>(id)];
                    const UiOverlayInput kind = overlay_input_kind(ov);
                    if (ov.visible && ov.m5.focused &&
                        (kind == UiOverlayInput::TextBox || kind == UiOverlayInput::NumberBox)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

private:
    // One `mbPresentationContainer` of the mission (conversation) window `0xc`.
    struct MissionPresentation {
        std::int32_t no = -1;
        std::uint32_t flags = 0;
        SimpleTriggerManager triggers{};
        std::vector<WsePresentationOverlay> overlays{};
        std::unordered_map<std::int32_t, WseM6OverlayExt> overlay_ext{};
        std::int32_t duration_cs = 0;
        float cs_accum = 0.0f;
        float elapsed_s = 0.0f;
        // Swap slots (see `swap_mission_presentation`). `vm_no` is the number the VM sees while
        // the container is swapped in; `no` itself never changes, so a trigger that calls
        // `start_presentation` 900 in a scene cannot renumber the container.
        std::int32_t vm_no = -1;
        std::int32_t window = kScreenWindowPresentation;
        std::int32_t focused_overlay = -1;
        UiWidget tree{};
        std::vector<std::int32_t> hovered{};
        std::int32_t armed = -1;
        std::int32_t drag = -1;
        float drag_grab = 0.0f;
        float drag_last = 0.0f;
        std::int32_t hold = -1;
        float hold_start = 0.0f;
        float hold_last = 0.0f;
        std::int32_t popup = -1;
        std::vector<UiRect> popup_items{};
        std::unordered_map<std::int32_t, float> scroll{};
        std::int32_t scroll_drag_overlay = -1;
        float scroll_drag_press_y = 0.0f;
        float scroll_drag_last_y = 0.0f;
        bool scroll_drag_active = false;
    };
    std::vector<MissionPresentation> mission_stack_{};
    bool in_mission_container_ = false;  // a mission container is swapped in
    bool press_consumed_ = false;        // the last `handle_input` consumed its press

    // Makes `mp` the presentation the VM and the input code see (`g_presentationContainer`):
    // its overlays, number, duration, triggers, focus, widget tree and interactive state are
    // swapped into `world` / the session; calling it again swaps them back.
    void swap_mission_presentation(WseWorldState& world, MissionPresentation& mp) {
        world.overlays.swap(mp.overlays);
        world.m6.ui.overlay_ext.swap(mp.overlay_ext);
        std::swap(world.active_presentation_no, mp.vm_no);
        std::swap(world.presentation_duration, mp.duration_cs);
        std::swap(world.m5.focused_overlay_no, mp.focused_overlay);
        std::swap(active_triggers_, mp.triggers);
        std::swap(current_window, mp.window);
        std::swap(presentation_no_, mp.no);
        std::swap(root, mp.tree);
        hovered_overlays_.swap(mp.hovered);
        std::swap(armed_overlay_, mp.armed);
        std::swap(drag_overlay_, mp.drag);
        std::swap(drag_grab_offset_, mp.drag_grab);
        std::swap(drag_last_x_, mp.drag_last);
        std::swap(hold_overlay_, mp.hold);
        std::swap(hold_start_s_, mp.hold_start);
        std::swap(hold_last_s_, mp.hold_last);
        std::swap(popup_overlay_, mp.popup);
        popup_items_.swap(mp.popup_items);
        scroll_pos_.swap(mp.scroll);
        std::swap(scroll_drag_overlay_, mp.scroll_drag_overlay);
        std::swap(scroll_drag_press_y_, mp.scroll_drag_press_y);
        std::swap(scroll_drag_last_y_, mp.scroll_drag_last_y);
        std::swap(scroll_drag_active_, mp.scroll_drag_active);
        in_mission_container_ = !in_mission_container_;
        if (!in_mission_container_) {
            mp.vm_no = mp.no;  // drop a number a trigger wrote; the container keeps its own
        }
    }

    // The session state `update_mission_presentations` / `handle_mission_input` set aside while
    // the mission containers run (a mission has no campaign presentation open).
    struct CampaignPresentationState {
        std::int32_t no;
        std::int32_t duration;
        std::uint32_t serial;
        SimpleTriggerManager triggers;
    };
    CampaignPresentationState save_campaign_presentation(const WseWorldState& world) {
        CampaignPresentationState s{world.active_presentation_no, world.presentation_duration,
                                    world.m6.ui.presentation_start_serial, std::move(active_triggers_)};
        active_triggers_ = SimpleTriggerManager{};
        return s;
    }
    void restore_campaign_presentation(WseWorldState& world, CampaignPresentationState& s) {
        world.active_presentation_no = s.no;
        world.presentation_duration = s.duration;
        world.m6.ui.presentation_start_serial = s.serial;
        active_triggers_ = std::move(s.triggers);
    }

    // Draws the mission containers: `root` becomes a screen-sized container holding each
    // container's overlay tree in stack order (later = on top), as the conversation window
    // draws its `mbPresentationContainer` children (`mbConversationWindow::updatePresentations`
    // `0x1400888c0` adds each new container last).
    void compose_mission_root() {
        reset_root("mission");
        for (const MissionPresentation& mp : mission_stack_) {
            root.children.push_back(mp.tree);
        }
    }

    // Pointer and key input for the mission containers, top container first. Each container
    // runs the presentation input path of `handle_input` on its own overlays and triggers
    // (hover enter / leave reach every container); a press one container consumes is not
    // handed to the containers below it (`rglContainerWidget::triggerState` `0x14028af30`).
    std::vector<UiEvent> handle_mission_input(WseWorldState& world,
                                              const UiPointerInput& in,
                                              vm::VmContext* vm_ctx,
                                              std::span<const LoadedPresentation> presentations) {
        std::vector<UiEvent> events;
        CampaignPresentationState saved = save_campaign_presentation(world);
        UiPointerInput rest = in;
        for (std::size_t k = mission_stack_.size(); k > 0U; --k) {
            MissionPresentation& mp = mission_stack_[k - 1U];
            swap_mission_presentation(world, mp);
            press_consumed_ = false;
            std::vector<UiEvent> ev = handle_input(world, rest, vm_ctx, presentations);
            const bool consumed = press_consumed_;
            swap_mission_presentation(world, mp);
            events.insert(events.end(), ev.begin(), ev.end());
            if (consumed) {
                rest.left_pressed = false;
                rest.text_input.clear();
            }
        }
        press_consumed_ = false;
        restore_campaign_presentation(world, saved);
        compose_mission_root();
        return events;
    }

    // `mbConversationWindow::updatePresentations` (`0x1400888c0`), once per mission frame:
    // 1. each pending `start_conversation_presentation` ends every open container that shows the
    //    same presentation or lacks `prsntf_manual_end_only` (`kPresentationFlagManualEndOnly`,
    //    container `+0x1c`), then opens a new container and starts it (`-60`,
    //    `mbPresentationContainer::startPresentation` `0x1402097b0`);
    // 2. every container runs its frame (`-61` with the elapsed milliseconds,
    //    `mbPresentationContainer::frameMove` `0x140208ec0`) and closes once its duration ran out.
    // `world.m6.ui.active_presentations` lists the open containers afterwards (read by
    // `presentation_activate` 4901 and the mission branch of `is_presentation_active` 903).
    // Afterwards each container's overlay tree is rebuilt and `root` shows them all
    // (`compose_mission_root`); `handle_input` routes the pointer to them.
    void update_mission_presentations(WseWorldState& world,
                                      vm::VmContext& vm_ctx,
                                      float dt_s,
                                      std::span<const LoadedPresentation> presentations) {
        const float dt = (std::isfinite(dt_s) && dt_s > 0.0f) ? dt_s : 0.0f;
        // The campaign presentation state, restored afterwards (a mission has none open).
        CampaignPresentationState saved = save_campaign_presentation(world);

        for (const auto& [no, cs] : world.m6.ui.mission_duration_requests) {
            for (MissionPresentation& mp : mission_stack_) {
                if (mp.no == no) {
                    mp.duration_cs = cs;
                    break;
                }
            }
        }
        world.m6.ui.mission_duration_requests.clear();

        std::vector<std::int32_t> pending;
        pending.swap(world.m6.ui.pending_conversation_presentations);
        for (const std::int32_t p : pending) {
            if (p < 0 || static_cast<std::size_t>(p) >= presentations.size()) {
                continue;
            }
            std::erase_if(mission_stack_, [p](const MissionPresentation& mp) {
                return mp.no == p || (mp.flags & kPresentationFlagManualEndOnly) == 0U;
            });
            MissionPresentation mp{};
            mp.no = p;
            mp.vm_no = p;
            mp.flags = presentations[static_cast<std::size_t>(p)].flags;
            mp.triggers = presentations[static_cast<std::size_t>(p)].triggers;
            // Read-only (flag bit 0) presentations default to 10.0 s, others to 100 s
            // (`mbPresentationContainer::startPresentation` `0x1402097b0`).
            mp.duration_cs = ((mp.flags & 1U) != 0U) ? 1000 : 10000;
            mission_stack_.push_back(std::move(mp));
            MissionPresentation& added = mission_stack_.back();
            swap_mission_presentation(world, added);
            const std::int32_t saved_container = world.m5.current_container_overlay_no;
            world.m5.current_container_overlay_no = -1;
            dispatch_presentation_trigger(world, vm_ctx, -60, {});
            world.m5.current_container_overlay_no = saved_container;
            swap_mission_presentation(world, added);
        }

        for (std::size_t i = 0; i < mission_stack_.size();) {
            MissionPresentation& mp = mission_stack_[i];
            mp.elapsed_s += dt;
            mp.cs_accum += dt * 100.0f;
            const float whole_cs = std::floor(mp.cs_accum);
            mp.cs_accum -= whole_cs;
            swap_mission_presentation(world, mp);
            const std::array<std::int64_t, 1> params{static_cast<std::int64_t>(mp.elapsed_s * 1000.0f)};
            dispatch_presentation_trigger(world, vm_ctx, -61, params);
            if (whole_cs > 0.0f && world.presentation_duration > 0) {
                const float left = static_cast<float>(world.presentation_duration) - whole_cs;
                world.presentation_duration = (left > 0.0f) ? static_cast<std::int32_t>(left) : 0;
            }
            animate_overlays(world, dt);
            swap_mission_presentation(world, mp);
            if (mp.duration_cs <= 0) {
                mission_stack_.erase(mission_stack_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }

        world.m6.ui.active_presentations.clear();
        for (MissionPresentation& mp : mission_stack_) {
            world.m6.ui.active_presentations.push_back(mp.no);
            swap_mission_presentation(world, mp);
            sync_overlays_from_world(world);
            swap_mission_presentation(world, mp);
        }
        restore_campaign_presentation(world, saved);
    }

    // `overlay_animate_to_*` (932..937) per `rglWidget::frameMove` (`0x14031c110`): each running
    // channel moves linearly from its start to its target, `(start * (d - t) + target * t) / d`;
    // colour bytes and alpha round half up (`floor`, +1 when the fraction is above 0.5); size and
    // position stay unrounded. Once `t` reaches the duration the target is set and the channel
    // stops. Port: time advances with the presentation frame time (`step_presentation`).
    static void animate_overlays(WseWorldState& world, float dt) {
        const auto lerp = [](float s, float g, float t, float d) {
            return (s * (d - t) + g * t) * (1.0f / d);
        };
        const auto round_half_up = [](float v) {
            const float f = std::floor(v);
            return static_cast<std::int32_t>(f) + ((v - f) > 0.5f ? 1 : 0);
        };
        const auto lerp_byte = [&](std::uint32_t s, std::uint32_t g, unsigned shift, float t, float d) {
            const auto sv = static_cast<float>((s >> shift) & 0xFFU);
            const auto gv = static_cast<float>((g >> shift) & 0xFFU);
            const std::int32_t v = round_half_up(lerp(sv, gv, t, d));
            return static_cast<std::uint32_t>(std::clamp(v, 0, 255)) << shift;
        };
        const auto lerp_rgb = [&](std::uint32_t s, std::uint32_t g, std::uint32_t keep, float t, float d) {
            return (keep & 0xFF000000U) | lerp_byte(s, g, 16U, t, d) | lerp_byte(s, g, 8U, t, d) |
                   lerp_byte(s, g, 0U, t, d);
        };
        const auto lerp_alpha = [&](float s, float g, float t, float d) {
            const std::int32_t v = round_half_up(lerp(s * 255.0f, g * 255.0f, t, d));
            return static_cast<float>(std::clamp(v, 0, 255)) / 255.0f;
        };
        for (WsePresentationOverlay& ov : world.overlays) {
            WseOverlayM5Ext& m = ov.m5;
            if (m.anim_active == 0U) {
                continue;
            }
            // Returns the elapsed time while the channel runs, or a negative value once it ended.
            const auto advance = [&m, dt](std::uint8_t bit, std::size_t slot, float dur) -> float {
                if ((m.anim_active & bit) == 0U) {
                    return -2.0f;
                }
                m.anim_elapsed[slot] += dt;
                const float t = m.anim_elapsed[slot];
                if (std::isfinite(dur) && dur > t) {
                    return t;
                }
                m.anim_active = static_cast<std::uint8_t>(m.anim_active & ~bit);
                return -1.0f;
            };
            if (const float t = advance(kOverlayAnimColor, 0, m.anim_color_duration); t >= 0.0f) {
                ov.color = lerp_rgb(m.anim_color_start, m.anim_color, ov.color, t, m.anim_color_duration);
            } else if (t == -1.0f) {
                ov.color = m.anim_color;
            }
            if (const float t = advance(kOverlayAnimAlpha, 1, m.anim_alpha_duration); t >= 0.0f) {
                ov.alpha = lerp_alpha(m.anim_alpha_start, m.anim_alpha, t, m.anim_alpha_duration);
            } else if (t == -1.0f) {
                ov.alpha = m.anim_alpha;
            }
            if (const float t = advance(kOverlayAnimHighlightColor, 2, m.anim_highlight_color_duration);
                t >= 0.0f) {
                m.highlight_color = lerp_rgb(m.anim_highlight_color_start, m.anim_highlight_color,
                                             m.highlight_color, t, m.anim_highlight_color_duration);
            } else if (t == -1.0f) {
                m.highlight_color = m.anim_highlight_color;
            }
            if (const float t = advance(kOverlayAnimHighlightAlpha, 3, m.anim_highlight_alpha_duration);
                t >= 0.0f) {
                m.highlight_alpha = lerp_alpha(m.anim_highlight_alpha_start, m.anim_highlight_alpha, t,
                                               m.anim_highlight_alpha_duration);
            } else if (t == -1.0f) {
                m.highlight_alpha = m.anim_highlight_alpha;
            }
            if (const float t = advance(kOverlayAnimSize, 4, m.anim_size_duration); t >= 0.0f) {
                ov.size = {lerp(m.anim_size_start.x, m.anim_size.x, t, m.anim_size_duration),
                           lerp(m.anim_size_start.y, m.anim_size.y, t, m.anim_size_duration)};
                m.size_set = true;
            } else if (t == -1.0f) {
                ov.size = m.anim_size;
                m.size_set = true;
            }
            if (const float t = advance(kOverlayAnimPosition, 5, m.anim_position_duration); t >= 0.0f) {
                ov.position = {lerp(m.anim_position_start.x, m.anim_position.x, t, m.anim_position_duration),
                               lerp(m.anim_position_start.y, m.anim_position.y, t, m.anim_position_duration)};
            } else if (t == -1.0f) {
                ov.position = m.anim_position;
            }
        }
    }

    // Interactive-overlay state (see `handle_input`).
    std::vector<std::int32_t> hovered_overlays_{};  // sorted
    std::int32_t armed_overlay_ = -1;
    std::string armed_widget_id_{};
    std::int32_t drag_overlay_ = -1;
    float drag_grab_offset_ = 0.0f;
    float drag_last_x_ = 0.0f;
    std::int32_t hold_overlay_ = -1;
    float hold_start_s_ = 0.0f;
    float hold_last_s_ = 0.0f;
    std::int32_t popup_overlay_ = -1;
    std::vector<UiRect> popup_items_{};
    std::unordered_map<std::int32_t, float> scroll_pos_{};
    std::int32_t scroll_drag_overlay_ = -1;
    float scroll_drag_press_y_ = 0.0f;
    float scroll_drag_last_y_ = 0.0f;
    bool scroll_drag_active_ = false;
    // Bumped by every presentation (re)start so a trigger that restarts it stops the dispatch.
    std::uint32_t presentation_epoch_ = 0;

    [[nodiscard]] static std::size_t utf16_code_points(std::u16string_view s) noexcept {
        std::size_t n = 0;
        for (const char16_t c : s) {
            n += (c >= 0xDC00U && c <= 0xDFFFU) ? 0U : 1U;
        }
        return n;
    }

    // The focused text box (`overlay_obtain_focus` 949 or a press), or -1.
    [[nodiscard]] static std::int32_t focused_text_box(const WseWorldState& world) noexcept {
        const std::int32_t id = world.m5.focused_overlay_no;
        if (id < 0 || static_cast<std::size_t>(id) >= world.overlays.size()) {
            return -1;
        }
        const WsePresentationOverlay& ov = world.overlays[static_cast<std::size_t>(id)];
        const UiOverlayInput kind = overlay_input_kind(ov);
        return (ov.visible && ov.m5.focused &&
                (kind == UiOverlayInput::TextBox || kind == UiOverlayInput::NumberBox))
                   ? id
                   : -1;
    }

    void reset_input_state() noexcept {
        hovered_overlays_.clear();
        armed_overlay_ = -1;
        armed_widget_id_.clear();
        drag_overlay_ = -1;
        hold_overlay_ = -1;
        popup_overlay_ = -1;
        popup_items_.clear();
        scroll_pos_.clear();
        scroll_drag_overlay_ = -1;
        scroll_drag_press_y_ = 0.0f;
        scroll_drag_last_y_ = 0.0f;
        scroll_drag_active_ = false;
    }

    // Absolute boxes of the visible, enabled overlays in draw order (later = on top), clipped by
    // their scrollable containers. A disabled overlay hides its whole subtree from input, as
    // `rglContainerWidget::triggerState` (`0x14028af30`) skips disabled / hidden children.
    static void collect_overlay_hits(const UiWidget& node,
                                     float px,
                                     float py,
                                     const UiRect& clip,
                                     std::vector<UiHitEntry>& out) {
        for (const UiWidget& c : node.children) {
            if (!c.visible || !c.enabled) {
                continue;
            }
            const float ax = px + c.pos.x;
            const float ay = py + c.pos.y;
            const UiRect box{ax, ay, ax + c.size.x, ay + c.size.y};
            if (c.overlay_id >= 0 && c.size.x > 0.0f && c.size.y > 0.0f) {
                out.push_back(UiHitEntry{c.overlay_id, box, clip, c.clip_children});
            }
            const UiRect child_clip = c.clip_children ? clip.intersect(box) : clip;
            collect_overlay_hits(c, ax + c.child_offset.x, ay + c.child_offset.y, child_clip, out);
        }
    }

    [[nodiscard]] static const UiHitEntry* find_hit_entry(const std::vector<UiHitEntry>& hits,
                                                          std::int32_t id) noexcept {
        for (const UiHitEntry& e : hits) {
            if (e.overlay_id == id) {
                return &e;
            }
        }
        return nullptr;
    }

    // `rglSliderWidget::setAbsolutePosition` (`0x14030da60`): the value under `x`, clamped to
    // the bounds (minimum first, as retail compares), stored truncated toward zero (`-62` reads
    // the float value with `cvttss2si`).
    static void set_slider_from_cursor(WsePresentationOverlay& ov, const UiHitEntry& e, float x) {
        const float span = (e.box.x1 - e.box.x0) - 2.0f * kUiSliderMargin;
        if (!(span > 0.0f)) {
            return;
        }
        const auto lo = static_cast<float>(ov.m5.min_value);
        const auto hi = static_cast<float>(ov.m5.max_value);
        float v = lo + (x - e.box.x0 - kUiSliderMargin) / span * (hi - lo);
        if (lo > v) {
            v = lo;
        } else if (v > hi) {
            v = hi;
        }
        if (!std::isfinite(v)) {
            return;
        }
        // Safety deviation: saturate instead of the undefined out-of-range conversion.
        v = std::clamp(v, -2147483520.0f, 2147483520.0f);
        ov.val = static_cast<std::int32_t>(std::trunc(v));
    }

    // `rglTextBoxWidget::setIntValue` (`0x140316a10`) in numeric mode, which a number box's inner
    // text box always is (`rglNumberBoxWidget::updateWidgets` `0x1402c8670` sets `+0x298` and
    // copies the range): with `max > min` the value clamps to `[min, max - 1]` (the module
    // header's "max value is excluded"); with an empty range it becomes -1 and the text clears.
    // `min_override` replaces `min` (typing clamps with a minimum of 0). The text mirrors the
    // value (`rglString::parse`).
    static void set_number_box_value(WsePresentationOverlay& ov,
                                     std::int64_t v,
                                     std::optional<std::int64_t> min_override = std::nullopt) {
        const std::int64_t lo = min_override.value_or(static_cast<std::int64_t>(ov.m5.min_value));
        const std::int64_t hi = static_cast<std::int64_t>(ov.m5.max_value);
        if (hi - lo > 0) {
            ov.val = static_cast<std::int32_t>(std::clamp<std::int64_t>(v, lo, hi - 1));
            const std::string digits = std::to_string(ov.val);
            ov.text.assign(digits.begin(), digits.end());
        } else {
            ov.val = -1;
            ov.text.clear();
        }
    }

    // `strtol(text, nullptr, 10)` as the numeric text box reads its text (0 when empty),
    // saturating where retail's 32-bit result would wrap.
    [[nodiscard]] static std::int64_t number_box_text_value(std::u16string_view t) noexcept {
        std::size_t i = 0;
        while (i < t.size() && (t[i] == u' ' || t[i] == u'\t')) {
            ++i;
        }
        bool neg = false;
        if (i < t.size() && (t[i] == u'-' || t[i] == u'+')) {
            neg = t[i] == u'-';
            ++i;
        }
        std::int64_t v = 0;
        for (; i < t.size() && t[i] >= u'0' && t[i] <= u'9'; ++i) {
            v = std::min<std::int64_t>(v * 10 + (t[i] - u'0'), std::int64_t{1} << 40);
        }
        return neg ? -v : v;
    }

    // `rglNumberBoxWidget::triggerState` (`0x1402c8310`): an arrow moves the value by `step`,
    // bounded by `max` going up and `min` going down, then `setIntValue` clamps it.
    static void step_number_box(WsePresentationOverlay& ov, std::int32_t step) {
        const std::int64_t lo = static_cast<std::int64_t>(ov.m5.min_value);
        const std::int64_t hi = static_cast<std::int64_t>(ov.m5.max_value);
        std::int64_t v = static_cast<std::int64_t>(ov.val) + step;
        v = step > 0 ? std::min(v, hi) : std::max(v, lo);
        set_number_box_value(ov, v);
    }

    // A left press on overlay `e`; returns whether the widget consumes it (the press then
    // reaches no overlay below).
    template <typename Raise>
    bool left_press(WseWorldState& world, const UiHitEntry& e, const UiPointerInput& in, Raise& raise) {
        WsePresentationOverlay& ov = world.overlays[static_cast<std::size_t>(e.overlay_id)];
        switch (overlay_input_kind(ov)) {
        case UiOverlayInput::Button:
            // `rglSimpleButtonWidget::triggerState` (`0x140307770`): arm on press, fire on release.
            armed_overlay_ = e.overlay_id;
            return true;
        case UiOverlayInput::CheckBox:
            // Same function, toggle path (flag `0x284` bit 0): flip and report at once.
            ov.val = (ov.val != 0) ? 0 : 1;
            raise(e.overlay_id, kUiStateChanged);
            return true;
        case UiOverlayInput::Slider: {
            // `rglSliderWidget::triggerState` (`0x14030d610`). Port: a press on the knob keeps
            // the knob under the cursor while dragging (the grab offset is not traced).
            const float span = (e.box.x1 - e.box.x0) - 2.0f * kUiSliderMargin;
            const double range = ov.m5.max_value - ov.m5.min_value;
            const float pct = (range != 0.0)
                                  ? static_cast<float>((ov.val - ov.m5.min_value) / range)
                                  : 0.0f;
            const float knob = e.box.x0 + kUiSliderMargin + pct * span;
            if (std::fabs(in.x - knob) < kUiSliderMargin + kUiSliderKnobTolerance) {
                drag_grab_offset_ = in.x - knob;
            } else {
                drag_grab_offset_ = 0.0f;
                set_slider_from_cursor(ov, e, in.x);
                raise(e.overlay_id, kUiStateChanged);
            }
            drag_overlay_ = e.overlay_id;
            drag_last_x_ = in.x;
            return true;
        }
        case UiOverlayInput::NumberBox:
            // `rglNumberBoxWidget::triggerState` (`0x1402c8310`): step 1 at once, then repeat.
            if (!e.on_left_arrow(in.x) && !e.on_right_arrow(in.x)) {
                // Between the arrows sits the numeric text box: a press focuses it
                // (`rglTextBoxWidget::triggerState` `0x140316bd0` -> `obtainFocus`).
                for (WsePresentationOverlay& other : world.overlays) {
                    other.m5.focused = false;
                }
                ov.m5.focused = true;
                world.m5.focused_overlay_no = e.overlay_id;
                if (ov.val == -1 && static_cast<std::int64_t>(ov.m5.max_value) -
                                            static_cast<std::int64_t>(ov.m5.min_value) <= 0) {
                    ov.text.clear();
                } else {
                    const std::string digits = std::to_string(ov.val);
                    ov.text.assign(digits.begin(), digits.end());
                }
                return true;
            }
            step_number_box(ov, e.on_right_arrow(in.x) ? 1 : -1);
            raise(e.overlay_id, kUiStateChanged);
            hold_overlay_ = e.overlay_id;
            hold_start_s_ = in.time_s;
            hold_last_s_ = 0.0f;
            return true;
        case UiOverlayInput::ComboLabel: {
            // `rglComboLabelWidget::triggerState` (`0x140262cd0`) steps by one and always raises
            // `0x10000`; `rglComboLabelWidget::setIntValue` (`0x140262c70`) wraps around the
            // item count and ignores the value when there are no items.
            if (!e.on_left_arrow(in.x) && !e.on_right_arrow(in.x)) {
                return false;
            }
            const auto count = static_cast<std::int32_t>(
                std::min<std::size_t>(ov.m5.items.size(), std::numeric_limits<std::int32_t>::max()));
            std::int32_t v = ov.val + (e.on_right_arrow(in.x) ? 1 : -1);
            if (v >= count) {
                v = 0;
            } else if (v < 0) {
                v = count - 1;
            }
            if (v >= 0 && v < count) {
                ov.val = v;
            }
            raise(e.overlay_id, kUiStateChanged);
            return true;
        }
        case UiOverlayInput::TextBox:
            // `rglTextBoxWidget::triggerState` (`0x140316bd0`) -> `obtainFocus` (`0x140317250`).
            for (WsePresentationOverlay& other : world.overlays) {
                other.m5.focused = false;
            }
            ov.m5.focused = true;
            world.m5.focused_overlay_no = e.overlay_id;
            return true;
        case UiOverlayInput::ComboButton:
        case UiOverlayInput::ListBox:
            // `rglComboButtonWidget::triggerState` (`0x140261340`): consumes the press and
            // opens the item popup when there are items; picking one raises `0x10000`.
            if (!ov.m5.items.empty()) {
                popup_overlay_ = e.overlay_id;
                popup_items_.clear();
                // `rglComboButtonWidget::openPopup` (`0x140261d10`): the list starts at the
                // box's bottom edge and grows downwards, one item per combo-box height
                // (`size.y * scale.y`).
                const float item_h = e.box.y1 - e.box.y0;
                for (std::size_t k = 0; k < ov.m5.items.size(); ++k) {
                    const float top = e.box.y0 - static_cast<float>(k) * item_h;
                    popup_items_.push_back(UiRect{e.box.x0, top - item_h, e.box.x1, top});
                }
            }
            return true;
        default:
            return false;
        }
    }

    // Wheel: scrolls the scrollable container closest to the cursor (distance 0 inside it),
    // as `mbPresentationContainer::frameMove` (`0x140208ec0`) picks it.
    void scroll_nearest_container(const std::vector<UiHitEntry>& hits, float x, float y, float wheel) {
        const auto axis_dist = [](float v, float lo, float hi) {
            if (v >= lo && v <= hi) {
                return 0.0f;
            }
            return std::min(std::fabs(v - lo), std::fabs(v - hi));
        };
        std::int32_t best = -1;
        float best_d = 1.0e10f;
        for (const UiHitEntry& e : hits) {
            if (!e.scrollable) {
                continue;
            }
            const float dx = axis_dist(x, e.box.x0, e.box.x1);
            const float dy = axis_dist(y, e.box.y0, e.box.y1);
            const float d = dx * dx + dy * dy;
            if (d < best_d) {
                best_d = d;
                best = e.overlay_id;
            }
        }
        if (best >= 0 && std::isfinite(wheel)) {
            // Clamped to the content on the next sync.
            scroll_pos_[best] = std::max(0.0f, scroll_pos_[best] - wheel * kUiScrollStep);
        }
    }

    [[nodiscard]] static std::int64_t changed_value(const WsePresentationOverlay& ov) noexcept {
        if (overlay_input_kind(ov) == UiOverlayInput::CheckBox) {
            return ov.val != 0 ? 1 : 0;
        }
        return ov.val;
    }

    // Runs the presentation triggers for the latched overlay states in retail order
    // (`mbPresentationContainer::frameMove` `0x140208ec0`), stopping when a trigger closes or
    // restarts the presentation.
    void dispatch_overlay_states(WseWorldState& world,
                                 vm::VmContext& vm_ctx,
                                 const std::map<std::int32_t, std::uint32_t>& states,
                                 std::span<const LoadedPresentation> presentations) {
        const std::uint32_t epoch = presentation_epoch_;
        struct Step {
            std::uint32_t bit;
            std::int32_t key;
            std::int64_t param2;  // -1: the overlay value
        };
        constexpr std::array<Step, 6> kOrder{{{kUiStateEnter, -63, 0},
                                              {kUiStateLeave, -63, 1},
                                              {kUiStateChanged, -62, -1},
                                              {kUiStateLeftPress, -64, 0},
                                              {kUiStateRightPress, -64, 1},
                                              {kUiStateMiddlePress, -64, 2}}};
        for (const auto& [id, bits] : states) {
            for (const Step& st : kOrder) {
                if ((bits & st.bit) == 0U) {
                    continue;
                }
                if (current_window != kScreenWindowPresentation || presentation_epoch_ != epoch ||
                    static_cast<std::size_t>(id) >= world.overlays.size()) {
                    return;
                }
                const std::int64_t p2 =
                    st.param2 >= 0 ? st.param2
                                   : changed_value(world.overlays[static_cast<std::size_t>(id)]);
                if (st.bit == kUiStateChanged &&
                    overlay_input_kind(world.overlays[static_cast<std::size_t>(id)]) ==
                        UiOverlayInput::TextBox) {
                    // `mbPresentationContainer::frameMove` (`0x140208ec0`) copies a text box's
                    // text into string register 0 (`mbBasicGame::m_stringRegisters` `+0x2670`).
                    vm_ctx.stringRegisters[0] = world.overlays[static_cast<std::size_t>(id)].text;
                }
                const std::array<std::int64_t, 2> params{id, p2};
                dispatch_presentation_trigger(world, vm_ctx, st.key, params);
                if (in_mission_container_) {
                    // A mission container closes from `update_mission_presentations` once its
                    // duration ran out; it does not switch presentations in place.
                    if (world.presentation_duration <= 0) {
                        return;
                    }
                    continue;
                }
                if (!follow_presentation_state(world, vm_ctx, presentations) ||
                    presentation_epoch_ != epoch) {
                    return;
                }
            }
        }
    }

    // Draws the open combo popup over everything (port approximation of `rglPopupWidget`).
    void append_combo_popup(const WseWorldState& world) {
        if (popup_overlay_ < 0 || static_cast<std::size_t>(popup_overlay_) >= world.overlays.size() ||
            popup_items_.empty()) {
            return;
        }
        const WsePresentationOverlay& ov = world.overlays[static_cast<std::size_t>(popup_overlay_)];
        UiWidget panel{};
        panel.id = "combo_popup";
        panel.kind = UiWidgetKind::SolidPanel;
        panel.pos = {popup_items_.back().x0, popup_items_.back().y0};
        panel.size = {popup_items_.front().x1 - popup_items_.front().x0,
                      popup_items_.front().y1 - popup_items_.back().y0};
        panel.color_argb = 0xE0101010U;
        for (std::size_t k = 0; k < popup_items_.size() && k < ov.m5.items.size(); ++k) {
            UiWidget item{};
            item.id = "combo_popup_item_" + std::to_string(k);
            item.kind = UiWidgetKind::Label;
            item.pos = {popup_items_[k].x0 - panel.pos.x, popup_items_[k].y0 - panel.pos.y};
            item.size = {popup_items_[k].x1 - popup_items_[k].x0,
                         popup_items_[k].y1 - popup_items_[k].y0};
            item.text = u16_to_utf8(ov.m5.items[k]);
            item.text_scale = {kUiOverlayTextScale, kUiOverlayTextScale};
            item.text_color_argb = (static_cast<std::int32_t>(k) == ov.val) ? 0xFFFFDD00U : 0xFFFFFFFFU;
            panel.children.push_back(std::move(item));
        }
        root.children.push_back(std::move(panel));
    }

    std::int32_t presentation_no_ = -1;
    float default_duration_s_ = 100.0f;
    float duration_cs_accum_ = 0.0f;
    // `world.m6.ui.menu_open_serial` as of the last menu `drive_frame` built.
    std::uint32_t seen_menu_serial_ = 0U;
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
        ++presentation_epoch_;
        reset_input_state();
        active_triggers_ = prsnt.triggers;
        presentation_elapsed_s = 0.0f;
        duration_cs_accum_ = 0.0f;
        // Overlay ids restart at 0 for every presentation.
        world.overlays.clear();
        world.m5.focused_overlay_no = -1;
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
        const bool inside_clip = !node.clip_children ||
                                 UiRect{ax, ay, ax + node.size.x, ay + node.size.y}.contains(ui_x, ui_y);
        if (inside_clip) {
            for (std::size_t k = node.children.size(); k > 0U; --k) {
                if (UiWidget* child_hit = find_hit_widget(node.children[k - 1U], ui_x, ui_y,
                                                          ax + node.child_offset.x,
                                                          ay + node.child_offset.y)) {
                    return child_hit;
                }
            }
        }
        if ((node.kind == UiWidgetKind::SimpleButton || node.kind == UiWidgetKind::GameButton) &&
            node.hit_test(ui_x, ui_y, px, py)) {
            return &node;
        }
        return nullptr;
    }

    // Updates the highlight / pressed visuals of the button under the cursor.
    static void update_hover_state(UiWidget& node, const UiWidget* target, bool left_down) {
        const bool now_hl = (&node == target);
        node.highlighted = now_hl;
        node.pressed = now_hl && left_down;
        for (UiWidget& c : node.children) {
            update_hover_state(c, target, left_down);
        }
    }

    static void clear_pressed_state(UiWidget& node) noexcept {
        node.pressed = false;
        for (UiWidget& c : node.children) {
            clear_pressed_state(c);
        }
    }
};

}  // namespace wse::game
