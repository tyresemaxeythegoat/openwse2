// Platform seam: input device abstraction, Warband raw/game-key mapping,
// context-sensitive touch overlay, layout editor, per-module profiles, and free
// camera controller (Slice P7, docs/PLATFORM_PLAN.md).
//
// Keeps `<SDL3/SDL.h>` out of engine headers (`platform/sdl/sdl_input.{h,cpp}`
// implements `InputBackend` for SDL3); `WseGameKeyManager` reconstructs WSE2
// v1.1.5.6 `rglInputManager` / `mbGameKeyManager` semantics from PDB/binary evidence:
//   - rglInputManager::getKeyName         0x1402ad5d0 (125 key names across 1..255)
//   - mbGameKeyManager::mbGameKeyManager  0x1400e96f0 (`g_gameKeyManager` 0x1406a0c00, 95 slots)
//   - mbGameKeyManager::reset             0x1400e9800 (75 default keys + Custom1..20 + 13 gamepad alts)
//   - mbGameKeyManager::loadKeys          0x1400ee440 / 0x1400ee4d0
//   - mbGameKeyManager::saveKeys          0x1400ee900 / 0x1400ee990
//   - mbGameKeyManager::keyDown           0x1400eef50 (primary + alt chord match)
//   - mbGameKeyManager::keyClicked        0x1400eeff0 (modifiers down, key down && !prev)
//   - mbGameKeyManager::keyRaised         0x1400ef0b0 (modifiers down, !key down && prev)
//   - mbGameKeyManager::defineBinding     0x1400ef170
//   - mbGameKeyManager::getKeyName        0x1400ef290
//   - mbGameKeyManager::getKeyData        0x1400ef2f0 (`mod0+mod1+key`, `ui_no_key_assigned`, `/`)
//   - mbGameKeyManager::storeKeys         0x1400ef500
//   - mbGameKeyManager::restoreKeys       0x1400ef5c0
//   - mbGameKeyManager::setKey            0x1400ef670
//
// Touch overlay, context visibility (`InputContext`), layout editor,
// `FreeCameraController`, and JSON profiles are port infrastructure (no retail
// equivalent).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/core/json.h"
#include "wse/core/math.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

inline constexpr std::int32_t kMaxRawInputKeys = 256;
inline constexpr std::int32_t kNumGameInputKeys = 95;  // 75 built-in + 20 Custom1..20
inline constexpr std::int32_t kNumBuiltInGameKeys = 75;

// DirectInput / Warband raw key indices matching `rglInputManager::getKeyName` (0x1402ad5d0).
namespace RawKey {
inline constexpr std::int32_t kEscape = 0x01;
inline constexpr std::int32_t k1 = 0x02;
inline constexpr std::int32_t k2 = 0x03;
inline constexpr std::int32_t k3 = 0x04;
inline constexpr std::int32_t k4 = 0x05;
inline constexpr std::int32_t k5 = 0x06;
inline constexpr std::int32_t k6 = 0x07;
inline constexpr std::int32_t k7 = 0x08;
inline constexpr std::int32_t k8 = 0x09;
inline constexpr std::int32_t k9 = 0x0a;
inline constexpr std::int32_t k0 = 0x0b;
inline constexpr std::int32_t kMinus = 0x0c;
inline constexpr std::int32_t kEquals = 0x0d;
inline constexpr std::int32_t kBackSpace = 0x0e;
inline constexpr std::int32_t kTab = 0x0f;
inline constexpr std::int32_t kQ = 0x10;
inline constexpr std::int32_t kW = 0x11;
inline constexpr std::int32_t kE = 0x12;
inline constexpr std::int32_t kR = 0x13;
inline constexpr std::int32_t kT = 0x14;
inline constexpr std::int32_t kY = 0x15;
inline constexpr std::int32_t kU = 0x16;
inline constexpr std::int32_t kI = 0x17;
inline constexpr std::int32_t kO = 0x18;
inline constexpr std::int32_t kP = 0x19;
inline constexpr std::int32_t kLBracket = 0x1a;
inline constexpr std::int32_t kRBracket = 0x1b;
inline constexpr std::int32_t kEnter = 0x1c;
inline constexpr std::int32_t kLeftCtrl = 0x1d;
inline constexpr std::int32_t kA = 0x1e;
inline constexpr std::int32_t kS = 0x1f;
inline constexpr std::int32_t kD = 0x20;
inline constexpr std::int32_t kF = 0x21;
inline constexpr std::int32_t kG = 0x22;
inline constexpr std::int32_t kH = 0x23;
inline constexpr std::int32_t kJ = 0x24;
inline constexpr std::int32_t kK = 0x25;
inline constexpr std::int32_t kL = 0x26;
inline constexpr std::int32_t kSemicolon = 0x27;
inline constexpr std::int32_t kApostrophe = 0x28;
inline constexpr std::int32_t kGrave = 0x29;
inline constexpr std::int32_t kLeftShift = 0x2a;
inline constexpr std::int32_t kBackslash = 0x2b;
inline constexpr std::int32_t kZ = 0x2c;
inline constexpr std::int32_t kX = 0x2d;
inline constexpr std::int32_t kC = 0x2e;
inline constexpr std::int32_t kV = 0x2f;
inline constexpr std::int32_t kB = 0x30;
inline constexpr std::int32_t kN = 0x31;
inline constexpr std::int32_t kM = 0x32;
inline constexpr std::int32_t kComma = 0x33;
inline constexpr std::int32_t kPeriod = 0x34;
inline constexpr std::int32_t kSlash = 0x35;
inline constexpr std::int32_t kRightShift = 0x36;
inline constexpr std::int32_t kNumpadStar = 0x37;
inline constexpr std::int32_t kLeftAlt = 0x38;
inline constexpr std::int32_t kSpace = 0x39;
inline constexpr std::int32_t kCapsLock = 0x3a;
inline constexpr std::int32_t kF1 = 0x3b;
inline constexpr std::int32_t kF2 = 0x3c;
inline constexpr std::int32_t kF3 = 0x3d;
inline constexpr std::int32_t kF4 = 0x3e;
inline constexpr std::int32_t kF5 = 0x3f;
inline constexpr std::int32_t kF6 = 0x40;
inline constexpr std::int32_t kF7 = 0x41;
inline constexpr std::int32_t kF8 = 0x42;
inline constexpr std::int32_t kF9 = 0x43;
inline constexpr std::int32_t kF10 = 0x44;
inline constexpr std::int32_t kNumLock = 0x45;
inline constexpr std::int32_t kNumpad7 = 0x47;
inline constexpr std::int32_t kNumpad8 = 0x48;
inline constexpr std::int32_t kNumpad9 = 0x49;
inline constexpr std::int32_t kNumpadMinus = 0x4a;
inline constexpr std::int32_t kNumpad4 = 0x4b;
inline constexpr std::int32_t kNumpad5 = 0x4c;
inline constexpr std::int32_t kNumpad6 = 0x4d;
inline constexpr std::int32_t kNumpadPlus = 0x4e;
inline constexpr std::int32_t kNumpad1 = 0x4f;
inline constexpr std::int32_t kNumpad2 = 0x50;
inline constexpr std::int32_t kNumpad3 = 0x51;
inline constexpr std::int32_t kNumpad0 = 0x52;
inline constexpr std::int32_t kNumpadDot = 0x53;
inline constexpr std::int32_t kF11 = 0x57;
inline constexpr std::int32_t kF12 = 0x58;
inline constexpr std::int32_t kNumpadEnter = 0x9c;
inline constexpr std::int32_t kRightCtrl = 0x9d;
inline constexpr std::int32_t kNumpadSlash = 0xb5;
inline constexpr std::int32_t kPrintScreen = 0xb7;
inline constexpr std::int32_t kRightAlt = 0xb8;
inline constexpr std::int32_t kHome = 0xc7;
inline constexpr std::int32_t kUpArrow = 0xc8;
inline constexpr std::int32_t kPageUp = 0xc9;
inline constexpr std::int32_t kLeftArrow = 0xcb;
inline constexpr std::int32_t kRightArrow = 0xcd;
inline constexpr std::int32_t kEnd = 0xcf;
inline constexpr std::int32_t kDownArrow = 0xd0;
inline constexpr std::int32_t kPageDown = 0xd1;
inline constexpr std::int32_t kInsert = 0xd2;
inline constexpr std::int32_t kDelete = 0xd3;
inline constexpr std::int32_t kLeftMouse = 0xe0;
inline constexpr std::int32_t kRightMouse = 0xe1;
inline constexpr std::int32_t kMiddleMouse = 0xe2;
inline constexpr std::int32_t kMouseBtn4 = 0xe3;
inline constexpr std::int32_t kMouseBtn5 = 0xe4;
inline constexpr std::int32_t kMouseBtn6 = 0xe5;
inline constexpr std::int32_t kMouseBtn7 = 0xe6;
inline constexpr std::int32_t kMouseBtn8 = 0xe7;
inline constexpr std::int32_t kScrollUp = 0xee;
inline constexpr std::int32_t kScrollDown = 0xef;
inline constexpr std::int32_t kGamepadA = 0xf0;
inline constexpr std::int32_t kGamepadB = 0xf1;
inline constexpr std::int32_t kGamepadX = 0xf2;
inline constexpr std::int32_t kGamepadY = 0xf3;
inline constexpr std::int32_t kDpadUp = 0xf4;
inline constexpr std::int32_t kDpadDown = 0xf5;
inline constexpr std::int32_t kDpadRight = 0xf6;
inline constexpr std::int32_t kDpadLeft = 0xf7;
inline constexpr std::int32_t kGamepadStart = 0xf8;
inline constexpr std::int32_t kGamepadBack = 0xf9;
inline constexpr std::int32_t kRightBumper = 0xfa;
inline constexpr std::int32_t kLeftBumper = 0xfb;
inline constexpr std::int32_t kLeftTrigger = 0xfc;
inline constexpr std::int32_t kRightTrigger = 0xfd;
inline constexpr std::int32_t kRightStick = 0xfe;
inline constexpr std::int32_t kLeftStick = 0xff;
}  // namespace RawKey

// Exact `rglInputManager::getKeyName` (`0x1402ad5d0`) table for raw key codes `0..255`.
[[nodiscard]] inline std::string_view get_raw_key_name(std::int32_t raw_key) noexcept {
    switch (raw_key) {
    case 0x01: return "Esc";
    case 0x02: return "1";
    case 0x03: return "2";
    case 0x04: return "3";
    case 0x05: return "4";
    case 0x06: return "5";
    case 0x07: return "6";
    case 0x08: return "7";
    case 0x09: return "8";
    case 0x0a: return "9";
    case 0x0b: return "0";
    case 0x0c: return "-";
    case 0x0d: return "=";
    case 0x0e: return "Back Space";
    case 0x0f: return "Tab";
    case 0x10: return "Q";
    case 0x11: return "W";
    case 0x12: return "E";
    case 0x13: return "R";
    case 0x14: return "T";
    case 0x15: return "Y";
    case 0x16: return "U";
    case 0x17: return "I";
    case 0x18: return "O";
    case 0x19: return "P";
    case 0x1a: return "[";
    case 0x1b: return "]";
    case 0x1c: return "Enter";
    case 0x1d: return "Left Ctrl";
    case 0x1e: return "A";
    case 0x1f: return "S";
    case 0x20: return "D";
    case 0x21: return "F";
    case 0x22: return "G";
    case 0x23: return "H";
    case 0x24: return "J";
    case 0x25: return "K";
    case 0x26: return "L";
    case 0x27: return ";";
    case 0x28: return "'";
    case 0x29: return "~";
    case 0x2a: return "Left Shift";
    case 0x2b: return "\\";
    case 0x2c: return "Z";
    case 0x2d: return "X";
    case 0x2e: return "C";
    case 0x2f: return "V";
    case 0x30: return "B";
    case 0x31: return "N";
    case 0x32: return "M";
    case 0x33: return ",";
    case 0x34: return ".";
    case 0x35: return "/";
    case 0x36: return "Right Shift";
    case 0x37: return "Numpad *";
    case 0x38: return "Left Alt";
    case 0x39: return "Space";
    case 0x3a: return "Caps Lock";
    case 0x3b: return "F1";
    case 0x3c: return "F2";
    case 0x3d: return "F3";
    case 0x3e: return "F4";
    case 0x3f: return "F5";
    case 0x40: return "F6";
    case 0x41: return "F7";
    case 0x42: return "F8";
    case 0x43: return "F9";
    case 0x44: return "F10";
    case 0x45: return "Num Lock";
    case 0x47: return "Numpad 7";
    case 0x48: return "Numpad 8";
    case 0x49: return "Numpad 9";
    case 0x4a: return "Numpad -";
    case 0x4b: return "Numpad 4";
    case 0x4c: return "Numpad 5";
    case 0x4d: return "Numpad 6";
    case 0x4e: return "Numpad +";
    case 0x4f: return "Numpad 1";
    case 0x50: return "Numpad 2";
    case 0x51: return "Numpad 3";
    case 0x52: return "Numpad 0";
    case 0x53: return "Numpad .";
    case 0x57: return "F11";
    case 0x58: return "F12";
    case 0x9c: return "Numpad Enter";
    case 0x9d: return "Right Ctrl";
    case 0xb5: return "Numpad /";
    case 0xb7: return "Print Screen";
    case 0xb8: return "Right Alt";
    case 0xc7: return "Home";
    case 0xc8: return "Up Arrow";
    case 0xc9: return "Page Up";
    case 0xcb: return "Left Arrow";
    case 0xcd: return "Right Arrow";
    case 0xcf: return "End";
    case 0xd0: return "Down Arrow";
    case 0xd1: return "Page Down";
    case 0xd2: return "Insert";
    case 0xd3: return "Delete";
    case 0xe0: return "Left Mouse Btn";
    case 0xe1: return "Right Mouse Btn";
    case 0xe2: return "Middle Mouse Btn";
    case 0xe3: return "Mouse Btn 4";
    case 0xe4: return "Mouse Btn 5";
    case 0xe5: return "Mouse Btn 6";
    case 0xe6: return "Mouse Btn 7";
    case 0xe7: return "Mouse Btn 8";
    case 0xee: return "Scroll Up";
    case 0xef: return "Scroll Down";
    case 0xf0: return "Gamepad A";
    case 0xf1: return "Gamepad B";
    case 0xf2: return "Gamepad X";
    case 0xf3: return "Gamepad Y";
    case 0xf4: return "D-pad Up";
    case 0xf5: return "D-pad Down";
    case 0xf6: return "D-pad Right";
    case 0xf7: return "D-pad Left";
    case 0xf8: return "Gamepad Start";
    case 0xf9: return "Gamepad Back";
    case 0xfa: return "Right Bumper";
    case 0xfb: return "Left Bumper";
    case 0xfc: return "Left Trigger";
    case 0xfd: return "Right Trigger";
    case 0xfe: return "Right Stick";
    case 0xff: return "Left Stick";
    default: return "";
    }
}

// Warband / WSE2 `gk_*` indices matching `mbGameKeyManager::reset` (`0x1400e9800`).
enum class GameKeyId : std::int32_t {
    MoveForward = 0,
    MoveBackward = 1,
    MoveLeft = 2,
    MoveRight = 3,
    Use = 4,
    Jump = 5,
    Attack = 6,
    Defend = 7,
    Kick = 8,
    ToggleWeaponMode = 9,
    EquipWeapon1 = 10,
    EquipWeapon2 = 11,
    EquipWeapon3 = 12,
    EquipWeapon4 = 13,
    EquipPrimaryWeapon = 14,
    EquipSecondaryWeapon = 15,
    DropPrimaryWeapon = 16,
    SheathPrimaryWeapon = 17,
    Leave = 18,
    Zoom = 19,
    FreeLook = 20,
    ToggleCamera = 21,
    ViewOrders = 22,
    Order1 = 23,
    Order2 = 24,
    Order3 = 25,
    Order4 = 26,
    Order5 = 27,
    Order6 = 28,
    ControlEveryone = 29,
    ControlGroup1 = 30,
    ControlGroup2 = 31,
    ControlGroup3 = 32,
    ControlGroup4 = 33,
    ControlGroup5 = 34,
    ControlGroup6 = 35,
    ControlGroup7 = 36,
    ControlGroup8 = 37,
    ControlGroup9 = 38,
    ControlReverse = 39,
    ControlEveryoneAround = 40,
    Chat = 41,
    TeamChat = 42,
    CharacterWindow = 43,
    InventoryWindow = 44,
    PartyWindow = 45,
    QuestsWindow = 46,
    GameLogWindow = 47,
    QuickSave = 48,
    Crouch = 49,
    Order7 = 50,
    Order8 = 51,
    Walk = 52,
    WalkToggle = 53,
    RearHorse = 54,
    HideUi = 55,
    HideTooltips = 56,
    SheathSecondaryWeapon = 57,
    DropSecondaryWeapon = 58,
    Dismount = 59,
    CommandLine = 60,
    CameraCloser = 61,
    CameraFarther = 62,
    SpectatorCameraMoveForward = 63,
    SpectatorCameraMoveBackward = 64,
    SpectatorCameraMoveLeft = 65,
    SpectatorCameraMoveRight = 66,
    SpectatorCameraFaster = 67,
    SpectatorCameraSlower = 68,
    MapMoveForward = 69,
    MapMoveBackward = 70,
    MapMoveLeft = 71,
    MapMoveRight = 72,
    MapWait = 73,
    TakeScreenshot = 74,
    Custom1 = 75,
    Custom2 = 76,
    Custom3 = 77,
    Custom4 = 78,
    Custom5 = 79,
    Custom6 = 80,
    Custom7 = 81,
    Custom8 = 82,
    Custom9 = 83,
    Custom10 = 84,
    Custom11 = 85,
    Custom12 = 86,
    Custom13 = 87,
    Custom14 = 88,
    Custom15 = 89,
    Custom16 = 90,
    Custom17 = 91,
    Custom18 = 92,
    Custom19 = 93,
    Custom20 = 94,
};

// One 16-byte binding record inside a `g_gameKeyManager` (`0x1406a0c00`) 32-byte slot:
// `{key, num_modifiers, modifiers[0], modifiers[1]}`.
struct KeyChord {
    std::int32_t key = -1;
    std::int32_t num_modifiers = 0;
    std::array<std::int32_t, 2> modifiers{-1, -1};

    [[nodiscard]] bool valid() const noexcept {
        return key >= 0 && key < kMaxRawInputKeys;
    }

    static constexpr KeyChord make(std::int32_t main_key,
                                   std::int32_t mod0 = -1,
                                   std::int32_t mod1 = -1) noexcept {
        KeyChord out{};
        out.key = main_key;
        if (mod0 >= 0 && mod0 < kMaxRawInputKeys) {
            out.modifiers[0] = mod0;
            out.num_modifiers = 1;
            if (mod1 >= 0 && mod1 < kMaxRawInputKeys) {
                out.modifiers[1] = mod1;
                out.num_modifiers = 2;
            }
        }
        return out;
    }

    [[nodiscard]] bool operator==(const KeyChord&) const noexcept = default;
};

struct GameKeyBinding {
    std::int32_t id = -1;
    std::uint32_t group_mask = 0;
    KeyChord primary{};
    KeyChord alt{};
    std::string config_name{};
};

// Reconstructs `mbGameKeyManager::reset` (`0x1400e9800`) defaults across all 95 slots
// (`0..74` built-in controls incl. modifiers & gamepad alts + `75..94` `Custom1..Custom20`).
[[nodiscard]] inline std::array<GameKeyBinding, kNumGameInputKeys>
default_game_key_bindings() {
    struct Entry {
        std::int32_t id;
        std::uint32_t group_mask;
        std::int32_t key;
        std::int32_t mod0;
        std::int32_t alt_key;
        const char* name;
    };
    static constexpr std::array<Entry, kNumBuiltInGameKeys> kDefaults = {{
        {0, 3U, RawKey::kW, -1, -1, "MoveForward"},
        {1, 3U, RawKey::kS, -1, -1, "MoveBackward"},
        {2, 3U, RawKey::kA, -1, -1, "MoveLeft"},
        {3, 3U, RawKey::kD, -1, -1, "MoveRight"},
        {4, 3U, RawKey::kF, -1, RawKey::kGamepadY, "Use"},
        {5, 3U, RawKey::kSpace, -1, RawKey::kGamepadA, "Jump"},
        {6, 3U, RawKey::kLeftMouse, -1, RawKey::kRightTrigger, "Attack"},
        {7, 3U, RawKey::kRightMouse, -1, RawKey::kLeftTrigger, "Defend"},
        {8, 1U, RawKey::kE, -1, RawKey::kGamepadX, "Kick"},
        {9, 3U, RawKey::kX, -1, RawKey::kDpadDown, "ToggleWeaponMode"},
        {10, 3U, RawKey::kNumpad1, -1, -1, "EquipWeapon1"},
        {11, 3U, RawKey::kNumpad2, -1, -1, "EquipWeapon2"},
        {12, 3U, RawKey::kNumpad3, -1, -1, "EquipWeapon3"},
        {13, 3U, RawKey::kNumpad4, -1, -1, "EquipWeapon4"},
        {14, 3U, RawKey::kScrollUp, -1, RawKey::kRightBumper, "EquipPrimaryWeapon"},
        {15, 3U, RawKey::kScrollDown, -1, RawKey::kLeftBumper, "EquipSecondaryWeapon"},
        {16, 3U, RawKey::kG, -1, RawKey::kGamepadB, "DropPrimaryWeapon"},
        {17, 3U, RawKey::kBackslash, -1, -1, "SheathPrimaryWeapon"},
        {18, 3U, RawKey::kTab, -1, RawKey::kGamepadBack, "Leave"},
        {19, 3U, RawKey::kLeftShift, -1, RawKey::kRightStick, "Zoom"},
        {20, 3U, RawKey::kGrave, -1, -1, "FreeLook"},
        {21, 3U, RawKey::kR, -1, RawKey::kLeftStick, "ToggleCamera"},
        {22, 3U, RawKey::kBackSpace, -1, -1, "ViewOrders"},
        {23, 3U, RawKey::kF1, -1, -1, "Order1"},
        {24, 3U, RawKey::kF2, -1, -1, "Order2"},
        {25, 3U, RawKey::kF3, -1, -1, "Order3"},
        {26, 3U, RawKey::kF4, -1, -1, "Order4"},
        {27, 3U, RawKey::kF5, -1, -1, "Order5"},
        {28, 3U, RawKey::kF6, -1, -1, "Order6"},
        {29, 3U, RawKey::k0, -1, -1, "ControlEveryone"},
        {30, 3U, RawKey::k1, -1, -1, "ControlGroup1"},
        {31, 3U, RawKey::k2, -1, -1, "ControlGroup2"},
        {32, 3U, RawKey::k3, -1, -1, "ControlGroup3"},
        {33, 3U, RawKey::k4, -1, -1, "ControlGroup4"},
        {34, 3U, RawKey::k5, -1, -1, "ControlGroup5"},
        {35, 3U, RawKey::k6, -1, -1, "ControlGroup6"},
        {36, 3U, RawKey::k7, -1, -1, "ControlGroup7"},
        {37, 3U, RawKey::k8, -1, -1, "ControlGroup8"},
        {38, 3U, RawKey::k9, -1, -1, "ControlGroup9"},
        {39, 3U, RawKey::kEquals, -1, -1, "ControlReverse"},
        {40, 3U, RawKey::kMinus, -1, -1, "ControlEveryoneAround"},
        {41, 7U, RawKey::kT, -1, -1, "Chat"},
        {42, 7U, RawKey::kY, -1, -1, "TeamChat"},
        {43, 7U, RawKey::kC, -1, -1, "CharacterWindow"},
        {44, 7U, RawKey::kI, -1, -1, "InventoryWindow"},
        {45, 7U, RawKey::kP, -1, -1, "PartyWindow"},
        {46, 7U, RawKey::kQ, -1, RawKey::kDpadUp, "QuestsWindow"},
        {47, 7U, RawKey::kL, -1, -1, "GameLogWindow"},
        {48, 7U, RawKey::kF12, -1, -1, "QuickSave"},
        {49, 1U, RawKey::kZ, -1, -1, "Crouch"},
        {50, 3U, RawKey::kF7, -1, -1, "Order7"},
        {51, 3U, RawKey::kF8, -1, -1, "Order8"},
        {52, 3U, RawKey::kLeftCtrl, -1, -1, "Walk"},
        {53, 3U, -1, -1, -1, "WalkToggle"},
        {54, 2U, RawKey::kJ, RawKey::kLeftCtrl, -1, "RearHorse"},
        {55, 7U, RawKey::kH, RawKey::kLeftCtrl, -1, "HideUi"},
        {56, 7U, RawKey::kO, RawKey::kLeftCtrl, -1, "HideTooltips"},
        {57, 3U, RawKey::kApostrophe, -1, -1, "SheathSecondaryWeapon"},
        {58, 3U, RawKey::kH, -1, -1, "DropSecondaryWeapon"},
        {59, 2U, -1, -1, -1, "Dismount"},
        {60, 7U, -1, -1, -1, "CommandLine"},
        {61, 7U, RawKey::kNumpadPlus, -1, -1, "CameraCloser"},
        {62, 7U, RawKey::kNumpadMinus, -1, -1, "CameraFarther"},
        {63, 4U, RawKey::kW, -1, -1, "SpectatorCameraMoveForward"},
        {64, 4U, RawKey::kS, -1, -1, "SpectatorCameraMoveBackward"},
        {65, 4U, RawKey::kA, -1, -1, "SpectatorCameraMoveLeft"},
        {66, 4U, RawKey::kD, -1, -1, "SpectatorCameraMoveRight"},
        {67, 4U, RawKey::kLeftCtrl, -1, -1, "SpectatorCameraFaster"},
        {68, 4U, RawKey::kLeftShift, -1, -1, "SpectatorCameraSlower"},
        {69, 8U, RawKey::kW, -1, -1, "MapMoveForward"},
        {70, 8U, RawKey::kS, -1, -1, "MapMoveBackward"},
        {71, 8U, RawKey::kA, -1, -1, "MapMoveLeft"},
        {72, 8U, RawKey::kD, -1, -1, "MapMoveRight"},
        {73, 8U, RawKey::kSpace, -1, -1, "MapWait"},
        {74, 15U, RawKey::kPrintScreen, -1, -1, "TakeScreenshot"},
    }};

    std::array<GameKeyBinding, kNumGameInputKeys> out{};
    for (const Entry& e : kDefaults) {
        GameKeyBinding& dst = out[static_cast<std::size_t>(e.id)];
        dst.id = e.id;
        dst.group_mask = e.group_mask;
        dst.primary = KeyChord::make(e.key, e.mod0);
        dst.alt = KeyChord::make(e.alt_key);
        dst.config_name = e.name;
    }
    for (std::int32_t i = kNumBuiltInGameKeys; i < kNumGameInputKeys; ++i) {
        GameKeyBinding& dst = out[static_cast<std::size_t>(i)];
        dst.id = i;
        dst.group_mask = 0U;
        dst.config_name = "Custom" + std::to_string((i - kNumBuiltInGameKeys) + 1);
    }
    return out;
}

// Reconstructs `mbGameKeyManager` (`g_gameKeyManager` `0x1406a0c00`).
class WseGameKeyManager {
public:
    WseGameKeyManager() { reset(); }

    void reset() {
        bindings_ = default_game_key_bindings();
        stored_.reset();
        bump_revision();
    }

    // Process-wide unique stamp that changes whenever any binding changes, so
    // `WorldInputBridge` can skip re-formatting 190 key names every frame.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] const GameKeyBinding* get(std::int32_t gk) const noexcept {
        if (gk < 0 || gk >= kNumGameInputKeys) {
            return nullptr;
        }
        return &bindings_[static_cast<std::size_t>(gk)];
    }

    [[nodiscard]] GameKeyBinding* get_mut(std::int32_t gk) noexcept {
        if (gk < 0 || gk >= kNumGameInputKeys) {
            return nullptr;
        }
        return &bindings_[static_cast<std::size_t>(gk)];
    }

    [[nodiscard]] const std::array<GameKeyBinding, kNumGameInputKeys>& bindings() const noexcept {
        return bindings_;
    }

    // Mirrors `mbGameKeyManager::setKey` (`0x1400ef670`): writes either primary (`alt==0`)
    // or alt (`alt==1`) chord. Rejects invalid indices.
    bool set_key(std::int32_t gk, std::int32_t alt, const KeyChord& chord) noexcept {
        if (gk < 0 || gk >= kNumGameInputKeys || (alt != 0 && alt != 1)) {
            return false;
        }
        KeyChord sanitized{};
        if (chord.key >= 0 && chord.key < kMaxRawInputKeys) {
            sanitized.key = chord.key;
            const std::int32_t mod_count = std::clamp(chord.num_modifiers, 0, 2);
            for (std::int32_t m = 0; m < mod_count; ++m) {
                const std::int32_t mk = chord.modifiers[static_cast<std::size_t>(m)];
                if (mk >= 0 && mk < kMaxRawInputKeys) {
                    sanitized.modifiers[static_cast<std::size_t>(sanitized.num_modifiers++)] = mk;
                }
            }
        }
        auto& b = bindings_[static_cast<std::size_t>(gk)];
        if (alt == 0) {
            b.primary = sanitized;
        } else {
            b.alt = sanitized;
        }
        bump_revision();
        return true;
    }

    // Mirrors `mbGameKeyManager::storeKeys` (`0x1400ef500`) / `restoreKeys` (`0x1400ef5c0`).
    void store_keys() { stored_ = bindings_; }
    bool restore_keys() {
        if (!stored_.has_value()) {
            return false;
        }
        bindings_ = *stored_;
        bump_revision();
        return true;
    }

    // Mirrors `mbGameKeyManager::keyDown` (`0x1400eef50`).
    [[nodiscard]] bool key_down(std::int32_t gk,
                                const std::array<bool, kMaxRawInputKeys>& raw_down) const noexcept {
        const GameKeyBinding* b = get(gk);
        if (b == nullptr) {
            return false;
        }
        return chord_matches(b->primary, raw_down, nullptr, Mode::Down) ||
               chord_matches(b->alt, raw_down, nullptr, Mode::Down);
    }

    // Mirrors `mbGameKeyManager::keyClicked` (`0x1400eeff0`): modifiers held now, main key rose.
    [[nodiscard]] bool key_clicked(
        std::int32_t gk,
        const std::array<bool, kMaxRawInputKeys>& raw_down,
        const std::array<bool, kMaxRawInputKeys>& raw_prev_down) const noexcept {
        const GameKeyBinding* b = get(gk);
        if (b == nullptr) {
            return false;
        }
        return chord_matches(b->primary, raw_down, &raw_prev_down, Mode::Clicked) ||
               chord_matches(b->alt, raw_down, &raw_prev_down, Mode::Clicked);
    }

    // Mirrors `mbGameKeyManager::keyRaised` (`0x1400ef0b0`): modifiers held now, main key released.
    [[nodiscard]] bool key_raised(
        std::int32_t gk,
        const std::array<bool, kMaxRawInputKeys>& raw_down,
        const std::array<bool, kMaxRawInputKeys>& raw_prev_down) const noexcept {
        const GameKeyBinding* b = get(gk);
        if (b == nullptr) {
            return false;
        }
        return chord_matches(b->primary, raw_down, &raw_prev_down, Mode::Raised) ||
               chord_matches(b->alt, raw_down, &raw_prev_down, Mode::Raised);
    }

    // Mirrors `mbGameKeyManager::getKeyData` (`0x1400ef2f0`) / `getKeyName` (`0x1400ef290`).
    [[nodiscard]] std::string get_key_data(
        std::int32_t gk,
        std::int32_t alt,
        bool& out_unassigned,
        std::string_view no_key_label = "No key assigned") const {
        out_unassigned = true;
        const GameKeyBinding* b = get(gk);
        if (b == nullptr || (alt != 0 && alt != 1)) {
            return {};
        }
        const KeyChord& c = (alt == 0) ? b->primary : b->alt;
        if (!c.valid()) {
            return (alt == 0) ? std::string(no_key_label) : std::string("/");
        }
        out_unassigned = false;
        std::string out;
        const std::int32_t mods = std::clamp(c.num_modifiers, 0, 2);
        for (std::int32_t m = 0; m < mods; ++m) {
            const std::int32_t mk = c.modifiers[static_cast<std::size_t>(m)];
            const std::string_view mn = get_raw_key_name(mk);
            if (!mn.empty()) {
                out.append(mn);
            }
            out.push_back('+');
        }
        out.append(get_raw_key_name(c.key));
        return out;
    }

    [[nodiscard]] std::string get_key_name(
        std::int32_t gk,
        std::int32_t alt,
        std::string_view no_key_label = "No key assigned") const {
        bool ignored = false;
        return get_key_data(gk, alt, ignored, no_key_label);
    }

    [[nodiscard]] std::int32_t find_by_config_name(std::string_view name) const noexcept {
        for (const auto& b : bindings_) {
            if (b.config_name == name) {
                return b.id;
            }
        }
        return -1;
    }

private:
    enum class Mode : std::uint8_t { Down, Clicked, Raised };

    void bump_revision() noexcept {
        static std::atomic<std::uint64_t> next{1U};
        revision_ = next.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] static bool chord_matches(
        const KeyChord& chord,
        const std::array<bool, kMaxRawInputKeys>& down,
        const std::array<bool, kMaxRawInputKeys>* prev,
        Mode mode) noexcept {
        if (!chord.valid()) {
            return false;
        }
        const std::int32_t mods = std::clamp(chord.num_modifiers, 0, 2);
        for (std::int32_t i = 0; i < mods; ++i) {
            const std::int32_t mk = chord.modifiers[static_cast<std::size_t>(i)];
            if (mk < 0 || mk >= kMaxRawInputKeys || !down[static_cast<std::size_t>(mk)]) {
                return false;
            }
        }
        const auto k = static_cast<std::size_t>(chord.key);
        switch (mode) {
        case Mode::Down:
            return down[k];
        case Mode::Clicked:
            return prev != nullptr && down[k] && !(*prev)[k];
        case Mode::Raised:
            return prev != nullptr && !down[k] && (*prev)[k];
        }
        return false;
    }

    std::array<GameKeyBinding, kNumGameInputKeys> bindings_{};
    std::optional<std::array<GameKeyBinding, kNumGameInputKeys>> stored_{};
    std::uint64_t revision_ = 0U;
};

// -----------------------------------------------------------------------------
// Port infrastructure: Contexts, Touch Overlay, Layout Editor, Profiles, Devices
// -----------------------------------------------------------------------------

enum class InputContext : std::uint8_t {
    Mission = 0,
    CampaignMap = 1,
    Menu = 2,
    Inventory = 3,
    Presentation = 4,
    Dialog = 5,
    Party = 6,
    Trade = 7,
};

[[nodiscard]] constexpr bool context_allows_touch_overlay(InputContext ctx) noexcept {
    return ctx == InputContext::Mission || ctx == InputContext::CampaignMap ||
           ctx == InputContext::Dialog || ctx == InputContext::Party ||
           ctx == InputContext::Trade;
}

enum class TouchCombatDirection : std::uint8_t {
    None = 0,
    Up = 1,
    Down = 2,
    Left = 3,
    Right = 4,
};

// Appended after the P7 widgets (`0..4`) so saved v1 profiles preserve existing IDs.
enum class TouchWidgetId : std::uint8_t {
    MoveStick = 0,
    LookArea = 1,
    AttackZone = 2,
    BlockZone = 3,
    CommandRadial = 4,
    MapCenter = 5,
    MapFollow = 6,
    MapPanMode = 7,
    MapWait = 8,
    DialogUp = 9,
    DialogSelect = 10,
    DialogDown = 11,
    MoveAll = 12,
    MoveOne = 13,
    Spare1 = 14,
    Spare2 = 15,
    Spare3 = 16,
    Spare4 = 17,
    Count = 18,
};

inline constexpr std::size_t kNumTouchWidgets =
    static_cast<std::size_t>(TouchWidgetId::Count);

[[nodiscard]] constexpr const char* touch_widget_key(TouchWidgetId id) noexcept {
    switch (id) {
    case TouchWidgetId::MoveStick: return "move_stick";
    case TouchWidgetId::LookArea: return "look_area";
    case TouchWidgetId::AttackZone: return "attack_zone";
    case TouchWidgetId::BlockZone: return "block_zone";
    case TouchWidgetId::CommandRadial: return "command_radial";
    case TouchWidgetId::MapCenter: return "map_center";
    case TouchWidgetId::MapFollow: return "map_follow";
    case TouchWidgetId::MapPanMode: return "map_pan_mode";
    case TouchWidgetId::MapWait: return "map_wait";
    case TouchWidgetId::DialogUp: return "dialog_up";
    case TouchWidgetId::DialogSelect: return "dialog_select";
    case TouchWidgetId::DialogDown: return "dialog_down";
    case TouchWidgetId::MoveAll: return "move_all";
    case TouchWidgetId::MoveOne: return "move_one";
    case TouchWidgetId::Spare1: return "spare_1";
    case TouchWidgetId::Spare2: return "spare_2";
    case TouchWidgetId::Spare3: return "spare_3";
    case TouchWidgetId::Spare4: return "spare_4";
    default: return "unknown";
    }
}

// Warband `GameKeyId` driven by each touch widget (`std::nullopt` for analog stick,
// look area, and multi-sector command radial).
// - `MapWait` uses retail `GameKeyId::MapWait` (73, `Space`).
// - `Spare1..4` bind to `GameKeyId::Custom1..4` (75..78).
// - Campaign map center/follow/pan, conversation up/select/down, and party/trade
//   bulk-move buttons have no built-in retail `GameKeyId` in `0..74`, so they use
//   spare slots `Custom5..12` (79..86).
[[nodiscard]] constexpr std::optional<GameKeyId> touch_widget_game_key(
    TouchWidgetId id) noexcept {
    switch (id) {
    case TouchWidgetId::AttackZone: return GameKeyId::Attack;
    case TouchWidgetId::BlockZone: return GameKeyId::Defend;
    case TouchWidgetId::MapCenter: return GameKeyId::Custom5;
    case TouchWidgetId::MapFollow: return GameKeyId::Custom6;
    case TouchWidgetId::MapPanMode: return GameKeyId::Custom7;
    case TouchWidgetId::MapWait: return GameKeyId::MapWait;
    case TouchWidgetId::DialogUp: return GameKeyId::Custom8;
    case TouchWidgetId::DialogSelect: return GameKeyId::Custom9;
    case TouchWidgetId::DialogDown: return GameKeyId::Custom10;
    case TouchWidgetId::MoveAll: return GameKeyId::Custom11;
    case TouchWidgetId::MoveOne: return GameKeyId::Custom12;
    case TouchWidgetId::Spare1: return GameKeyId::Custom1;
    case TouchWidgetId::Spare2: return GameKeyId::Custom2;
    case TouchWidgetId::Spare3: return GameKeyId::Custom3;
    case TouchWidgetId::Spare4: return GameKeyId::Custom4;
    default: return std::nullopt;
    }
}

[[nodiscard]] constexpr bool widget_visible_in_context(TouchWidgetId id,
                                                       InputContext ctx) noexcept {
    switch (ctx) {
    case InputContext::Mission:
        return id == TouchWidgetId::MoveStick || id == TouchWidgetId::LookArea ||
               id == TouchWidgetId::AttackZone || id == TouchWidgetId::BlockZone ||
               id == TouchWidgetId::CommandRadial || id == TouchWidgetId::Spare1 ||
               id == TouchWidgetId::Spare2 || id == TouchWidgetId::Spare3 ||
               id == TouchWidgetId::Spare4;
    case InputContext::CampaignMap:
        return id == TouchWidgetId::MoveStick || id == TouchWidgetId::LookArea ||
               id == TouchWidgetId::MapCenter || id == TouchWidgetId::MapFollow ||
               id == TouchWidgetId::MapPanMode || id == TouchWidgetId::MapWait;
    case InputContext::Dialog:
        return id == TouchWidgetId::DialogUp || id == TouchWidgetId::DialogSelect ||
               id == TouchWidgetId::DialogDown;
    case InputContext::Party:
    case InputContext::Trade:
        return id == TouchWidgetId::MoveAll || id == TouchWidgetId::MoveOne;
    case InputContext::Menu:
    case InputContext::Inventory:
    case InputContext::Presentation:
    default:
        return false;
    }
}

inline constexpr std::array<TouchWidgetId, kNumTouchWidgets> kTouchWidgetHitOrder = {
    TouchWidgetId::MoveStick,    TouchWidgetId::CommandRadial, TouchWidgetId::AttackZone,
    TouchWidgetId::BlockZone,    TouchWidgetId::MapCenter,     TouchWidgetId::MapFollow,
    TouchWidgetId::MapPanMode,   TouchWidgetId::MapWait,       TouchWidgetId::DialogUp,
    TouchWidgetId::DialogSelect, TouchWidgetId::DialogDown,    TouchWidgetId::MoveAll,
    TouchWidgetId::MoveOne,      TouchWidgetId::Spare1,        TouchWidgetId::Spare2,
    TouchWidgetId::Spare3,       TouchWidgetId::Spare4,        TouchWidgetId::LookArea,
};

// Viewport width / height. Touch coordinates are normalized per axis, so every
// distance or angle is converted to screen-height units (`dx * aspect`) first;
// otherwise circles become ellipses and swipes/radial sectors skew on wide screens.
[[nodiscard]] inline float sanitize_viewport_aspect(float aspect) noexcept {
    return (std::isfinite(aspect) && aspect >= 0.25f && aspect <= 4.0f) ? aspect : 1.0f;
}

// Normalized display safe-area insets in [0, 1] of viewport width / height.
struct SafeAreaInsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] bool operator==(const SafeAreaInsets&) const noexcept = default;
};

// Widget center (`cx`, `cy`) is in normalized viewport width / height `[0, 1]`;
// half-extents (`half_w`, `half_h`) are fractions of the screen's short side
// (`min(width, height)`), so circles stay circular and button proportions stay
// invariant across 20:9, 4:3, and portrait viewports.
struct TouchWidgetLayout {
    float cx = 0.5f;
    float cy = 0.5f;
    float half_w = 0.10f;
    float half_h = 0.12f;
    float scale = 1.0f;
    float opacity = 0.65f;
    bool circular = false;
    bool enabled = true;

    // Radius of a circular widget in screen-height units: short-side radius scaled
    // by `min(aspect, 1.0f)` so pixel radius equals `min(half_w, half_h) * scale * min(W, H)`.
    [[nodiscard]] float circle_radius(float aspect) const noexcept {
        const float a = sanitize_viewport_aspect(aspect);
        return std::max(0.01f, std::min(half_w, half_h) * scale) * std::min(a, 1.0f);
    }

    // Half-width in normalized viewport X `[0, 1]`.
    [[nodiscard]] float norm_half_width(float aspect) const noexcept {
        const float a = sanitize_viewport_aspect(aspect);
        if (circular) {
            return circle_radius(a) / a;
        }
        return std::max(0.01f, half_w * scale) * std::min(1.0f, 1.0f / a);
    }

    // Half-height in normalized viewport Y `[0, 1]`.
    [[nodiscard]] float norm_half_height(float aspect) const noexcept {
        const float a = sanitize_viewport_aspect(aspect);
        if (circular) {
            return circle_radius(a);
        }
        return std::max(0.01f, half_h * scale) * std::min(1.0f, a);
    }

    [[nodiscard]] bool contains(float x, float y, float aspect = 1.0f) const noexcept {
        if (!enabled || !std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }
        const float a = sanitize_viewport_aspect(aspect);
        const float dx = x - cx;
        const float dy = y - cy;
        if (circular) {
            const float r = circle_radius(a);
            const float px = dx * a;
            return (px * px + dy * dy) <= r * r;
        }
        const float eff_w = norm_half_width(a);
        const float eff_h = norm_half_height(a);
        return std::fabs(dx) <= eff_w && std::fabs(dy) <= eff_h;
    }

    [[nodiscard]] bool operator==(const TouchWidgetLayout&) const noexcept = default;
};

[[nodiscard]] constexpr std::array<TouchWidgetLayout, kNumTouchWidgets>
default_touch_widgets() noexcept {
    return {{
        TouchWidgetLayout{0.15f, 0.72f, 0.14f, 0.14f, 1.0f, 0.70f, true, true},   // MoveStick
        TouchWidgetLayout{0.68f, 0.50f, 0.31f, 0.42f, 1.0f, 0.25f, false, true},  // LookArea
        TouchWidgetLayout{0.76f, 0.68f, 0.10f, 0.11f, 1.0f, 0.75f, false, true},  // AttackZone
        TouchWidgetLayout{0.91f, 0.68f, 0.07f, 0.11f, 1.0f, 0.75f, false, true},  // BlockZone
        TouchWidgetLayout{0.15f, 0.28f, 0.09f, 0.09f, 1.0f, 0.70f, true, true},   // CommandRadial
        TouchWidgetLayout{0.54f, 0.12f, 0.055f, 0.055f, 1.0f, 0.75f, false, true},  // MapCenter
        TouchWidgetLayout{0.65f, 0.12f, 0.055f, 0.055f, 1.0f, 0.75f, false, true},  // MapFollow
        TouchWidgetLayout{0.76f, 0.12f, 0.055f, 0.055f, 1.0f, 0.75f, false, true},  // MapPanMode
        TouchWidgetLayout{0.87f, 0.12f, 0.055f, 0.055f, 1.0f, 0.75f, false, true},  // MapWait
        TouchWidgetLayout{0.90f, 0.36f, 0.06f, 0.06f, 1.0f, 0.75f, false, true},   // DialogUp
        TouchWidgetLayout{0.90f, 0.52f, 0.06f, 0.06f, 1.0f, 0.80f, false, true},   // DialogSelect
        TouchWidgetLayout{0.90f, 0.68f, 0.06f, 0.06f, 1.0f, 0.75f, false, true},   // DialogDown
        TouchWidgetLayout{0.90f, 0.42f, 0.065f, 0.06f, 1.0f, 0.75f, false, true},  // MoveAll
        TouchWidgetLayout{0.90f, 0.58f, 0.065f, 0.06f, 1.0f, 0.75f, false, true},  // MoveOne
        TouchWidgetLayout{0.48f, 0.11f, 0.05f, 0.05f, 1.0f, 0.65f, false, true},   // Spare1
        TouchWidgetLayout{0.59f, 0.11f, 0.05f, 0.05f, 1.0f, 0.65f, false, true},   // Spare2
        TouchWidgetLayout{0.70f, 0.11f, 0.05f, 0.05f, 1.0f, 0.65f, false, true},   // Spare3
        TouchWidgetLayout{0.81f, 0.11f, 0.05f, 0.05f, 1.0f, 0.65f, false, true},   // Spare4
    }};
}

[[nodiscard]] inline bool is_touch_widget_active(TouchWidgetId id,
                                                 InputContext ctx,
                                                 const TouchWidgetLayout& layout,
                                                 bool overlay_visible) noexcept {
    if (!overlay_visible || !layout.enabled) {
        return false;
    }
    return widget_visible_in_context(id, ctx);
}

struct TouchOverlayProfile {
    std::int32_t version = 1;
    float stick_deadzone = 0.15f;
    float stick_key_threshold = 0.35f;
    float gamepad_deadzone = 0.18f;
    float look_sensitivity = 2.5f;
    float swipe_threshold = 0.025f;
    float radial_deadzone = 0.02f;
    bool invert_look_y = false;
    bool touch_overlay_enabled = true;
    // Slice P8 sensor & gesture toggles (gyro aim is strictly opt-in).
    bool gyro_aim_enabled = false;
    float gyro_sensitivity = 1.0f;
    float gyro_smoothing = 0.35f;
    float gyro_deadzone = 0.02f;
    bool invert_gyro_y = false;
    bool pinch_zoom_enabled = true;
    float pinch_zoom_sensitivity = 1.0f;
    TouchCombatDirection default_attack_dir = TouchCombatDirection::Right;
    TouchCombatDirection default_block_dir = TouchCombatDirection::Up;
    std::array<TouchWidgetLayout, kNumTouchWidgets> widgets = default_touch_widgets();

    [[nodiscard]] bool operator==(const TouchOverlayProfile&) const noexcept = default;
};

inline void sanitize_touch_widget(TouchWidgetLayout& w) noexcept {
    const auto finite_or = [](float v, float def) noexcept {
        return std::isfinite(v) ? v : def;
    };
    w.cx = std::clamp(finite_or(w.cx, 0.5f), 0.04f, 0.96f);
    w.cy = std::clamp(finite_or(w.cy, 0.5f), 0.04f, 0.96f);
    w.half_w = std::clamp(finite_or(w.half_w, 0.10f), 0.03f, 0.48f);
    w.half_h = std::clamp(finite_or(w.half_h, 0.12f), 0.03f, 0.48f);
    w.scale = std::clamp(finite_or(w.scale, 1.0f), 0.5f, 2.0f);
    w.opacity = std::clamp(finite_or(w.opacity, 0.65f), 0.1f, 1.0f);
}

inline void sanitize_input_profile(TouchOverlayProfile& p) noexcept {
    const TouchOverlayProfile def{};
    const auto finite_or = [](float v, float fallback) noexcept {
        return std::isfinite(v) ? v : fallback;
    };
    p.version = 1;
    p.stick_deadzone = std::clamp(finite_or(p.stick_deadzone, def.stick_deadzone), 0.0f, 0.6f);
    p.stick_key_threshold =
        std::clamp(finite_or(p.stick_key_threshold, def.stick_key_threshold), 0.1f, 0.9f);
    p.gamepad_deadzone =
        std::clamp(finite_or(p.gamepad_deadzone, def.gamepad_deadzone), 0.0f, 0.6f);
    p.look_sensitivity =
        std::clamp(finite_or(p.look_sensitivity, def.look_sensitivity), 0.1f, 10.0f);
    p.swipe_threshold =
        std::clamp(finite_or(p.swipe_threshold, def.swipe_threshold), 0.005f, 0.25f);
    p.radial_deadzone =
        std::clamp(finite_or(p.radial_deadzone, def.radial_deadzone), 0.005f, 0.20f);
    p.gyro_sensitivity =
        std::clamp(finite_or(p.gyro_sensitivity, def.gyro_sensitivity), 0.1f, 5.0f);
    p.gyro_smoothing =
        std::clamp(finite_or(p.gyro_smoothing, def.gyro_smoothing), 0.0f, 0.95f);
    p.gyro_deadzone =
        std::clamp(finite_or(p.gyro_deadzone, def.gyro_deadzone), 0.0f, 0.5f);
    p.pinch_zoom_sensitivity =
        std::clamp(finite_or(p.pinch_zoom_sensitivity, def.pinch_zoom_sensitivity), 0.1f, 5.0f);
    const auto valid_dir = [](TouchCombatDirection d, TouchCombatDirection fallback) noexcept {
        const auto v = static_cast<std::uint8_t>(d);
        return (v >= 1U && v <= 4U) ? d : fallback;
    };
    p.default_attack_dir = valid_dir(p.default_attack_dir, def.default_attack_dir);
    p.default_block_dir = valid_dir(p.default_block_dir, def.default_block_dir);
    for (auto& w : p.widgets) {
        sanitize_touch_widget(w);
    }
}

// Clamps every widget rectangle inside the normalized safe-area inset rectangle
// `[insets.left, 1 - insets.right] x [insets.top, 1 - insets.bottom]`.
// With default `aspect = 1.0f` the clamped coordinates stay inside the safe area
// for any aspect ratio (since `norm_half_*` only shrinks away from `aspect = 1.0f`).
inline void apply_safe_area(TouchOverlayProfile& profile,
                            const SafeAreaInsets& insets,
                            float aspect = 1.0f) noexcept {
    const float a = sanitize_viewport_aspect(aspect);
    const auto finite_nonneg = [](float v) noexcept {
        return (std::isfinite(v) && v > 0.0f) ? v : 0.0f;
    };
    float l = std::min(finite_nonneg(insets.left), 0.98f);
    float r = std::min(finite_nonneg(insets.right), 0.98f);
    if (l + r > 0.98f) {
        const float k = 0.98f / (l + r);
        l *= k;
        r *= k;
    }
    float t = std::min(finite_nonneg(insets.top), 0.98f);
    float b = std::min(finite_nonneg(insets.bottom), 0.98f);
    if (t + b > 0.98f) {
        const float k = 0.98f / (t + b);
        t *= k;
        b *= k;
    }
    const float min_x = l;
    const float max_x = 1.0f - r;
    const float min_y = t;
    const float max_y = 1.0f - b;
    const float safe_w = max_x - min_x;
    const float safe_h = max_y - min_y;
    const float scale_x_to_short = std::min(1.0f, 1.0f / a);
    const float scale_y_to_short = std::min(1.0f, a);

    for (TouchWidgetLayout& w : profile.widgets) {
        sanitize_touch_widget(w);
        float hw = w.norm_half_width(a);
        float hh = w.norm_half_height(a);
        if (w.circular) {
            const float max_rx = safe_w * 0.5f;
            const float max_ry = safe_h * 0.5f;
            if (hw > max_rx || hh > max_ry) {
                const float factor = std::min(max_rx / hw, max_ry / hh);
                const float new_r_short = std::min(w.half_w, w.half_h) * w.scale * factor;
                const float s = std::max(w.scale, 0.5f);
                const float unclamped = new_r_short / s;
                w.half_w = std::max(0.005f, unclamped);
                w.half_h = std::max(0.005f, unclamped);
                hw = std::min(w.norm_half_width(a), max_rx);
                hh = std::min(w.norm_half_height(a), max_ry);
            }
        } else {
            if (2.0f * hw > safe_w) {
                const float max_hw_short = (safe_w * 0.5f) / scale_x_to_short;
                w.half_w = std::max(0.005f, max_hw_short / std::max(w.scale, 0.5f));
                hw = std::min(w.norm_half_width(a), safe_w * 0.5f);
            }
            if (2.0f * hh > safe_h) {
                const float max_hh_short = (safe_h * 0.5f) / scale_y_to_short;
                w.half_h = std::max(0.005f, max_hh_short / std::max(w.scale, 0.5f));
                hh = std::min(w.norm_half_height(a), safe_h * 0.5f);
            }
        }
        const float lo_x = min_x + hw;
        const float hi_x = max_x - hw;
        const float lo_y = min_y + hh;
        const float hi_y = max_y - hh;
        w.cx = (lo_x <= hi_x) ? std::clamp(w.cx, lo_x, hi_x) : 0.5f * (min_x + max_x);
        w.cy = (lo_y <= hi_y) ? std::clamp(w.cy, lo_y, hi_y) : 0.5f * (min_y + max_y);
        if (w.cx - hw < min_x && w.cx + hw < max_x) {
            w.cx = std::nextafter(w.cx, max_x);
        }
        if (w.cx + hw > max_x && w.cx - hw > min_x) {
            w.cx = std::nextafter(w.cx, min_x);
        }
        if (w.cy - hh < min_y && w.cy + hh < max_y) {
            w.cy = std::nextafter(w.cy, max_y);
        }
        if (w.cy + hh > max_y && w.cy - hh > min_y) {
            w.cy = std::nextafter(w.cy, min_y);
        }
    }
}

// Interactive/programmatic editor for `TouchOverlayProfile::widgets`.
class TouchLayoutEditor {
public:
    void begin_edit() noexcept { active_ = true; }
    void end_edit() noexcept { active_ = false; }
    [[nodiscard]] bool active() const noexcept { return active_; }

    void select(TouchWidgetId id) noexcept { selected_ = id; }
    [[nodiscard]] TouchWidgetId selected() const noexcept { return selected_; }

    void reset_defaults(TouchOverlayProfile& profile) const noexcept {
        profile.widgets = default_touch_widgets();
    }

    [[nodiscard]] TouchWidgetId hit_test(const TouchOverlayProfile& profile,
                                         float x,
                                         float y,
                                         float aspect = 1.0f) const noexcept {
        for (const TouchWidgetId id : kTouchWidgetHitOrder) {
            if (profile.widgets[static_cast<std::size_t>(id)].contains(x, y, aspect)) {
                return id;
            }
        }
        return TouchWidgetId::Count;
    }

    [[nodiscard]] TouchWidgetId hit_test(const TouchOverlayProfile& profile,
                                         InputContext ctx,
                                         float x,
                                         float y,
                                         float aspect = 1.0f) const noexcept {
        for (const TouchWidgetId id : kTouchWidgetHitOrder) {
            if (widget_visible_in_context(id, ctx) &&
                profile.widgets[static_cast<std::size_t>(id)].contains(x, y, aspect)) {
                return id;
            }
        }
        return TouchWidgetId::Count;
    }

    bool set_widget_position(TouchOverlayProfile& profile,
                             TouchWidgetId id,
                             float cx,
                             float cy) noexcept {
        if (id >= TouchWidgetId::Count || !std::isfinite(cx) || !std::isfinite(cy)) {
            return false;
        }
        auto& w = profile.widgets[static_cast<std::size_t>(id)];
        w.cx = cx;
        w.cy = cy;
        sanitize_touch_widget(w);
        return true;
    }

    bool move_selected(TouchOverlayProfile& profile, float dx, float dy) noexcept {
        if (!active_ || selected_ >= TouchWidgetId::Count || !std::isfinite(dx) ||
            !std::isfinite(dy)) {
            return false;
        }
        const auto& w = profile.widgets[static_cast<std::size_t>(selected_)];
        return set_widget_position(profile, selected_, w.cx + dx, w.cy + dy);
    }

    bool set_widget_scale(TouchOverlayProfile& profile, TouchWidgetId id, float scale) noexcept {
        if (id >= TouchWidgetId::Count || !std::isfinite(scale)) {
            return false;
        }
        auto& w = profile.widgets[static_cast<std::size_t>(id)];
        w.scale = scale;
        sanitize_touch_widget(w);
        return true;
    }

    bool set_widget_opacity(TouchOverlayProfile& profile,
                            TouchWidgetId id,
                            float opacity) noexcept {
        if (id >= TouchWidgetId::Count || !std::isfinite(opacity)) {
            return false;
        }
        auto& w = profile.widgets[static_cast<std::size_t>(id)];
        w.opacity = opacity;
        sanitize_touch_widget(w);
        return true;
    }

    bool set_widget_enabled(TouchOverlayProfile& profile,
                            TouchWidgetId id,
                            bool enabled) noexcept {
        if (id >= TouchWidgetId::Count) {
            return false;
        }
        profile.widgets[static_cast<std::size_t>(id)].enabled = enabled;
        return true;
    }

private:
    bool active_ = false;
    TouchWidgetId selected_ = TouchWidgetId::MoveStick;
};

namespace input_detail {

inline bool parse_float_exact(std::string_view s, float& out) noexcept {
    if (s.empty()) {
        return false;
    }
    float val = 0.0f;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec != std::errc{} || ptr != s.data() + s.size() || !std::isfinite(val)) {
        return false;
    }
    out = val;
    return true;
}

inline bool parse_int_exact(std::string_view s, std::int32_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    std::int32_t val = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), val);
    if (ec != std::errc{} || ptr != s.data() + s.size()) {
        return false;
    }
    out = val;
    return true;
}

inline bool parse_bool_exact(std::string_view s, bool& out) noexcept {
    if (s == "true" || s == "1") {
        out = true;
        return true;
    }
    if (s == "false" || s == "0") {
        out = false;
        return true;
    }
    return false;
}

// Locale-independent shortest round-trip form (`snprintf("%g")` follows the C
// locale's decimal point, which `from_chars` would then refuse to read back).
inline std::string fmt_float(float v) {
    char buf[48];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return (ec == std::errc{}) ? std::string(buf, ptr) : std::string("0");
}

}  // namespace input_detail

// Maps a module name onto a safe single-segment filename stem. ASCII letters fold
// to lower case (module folders are case-insensitive); `[a-z0-9_-]` and inner `.`
// are kept; every other byte (space, `&`, `'`, `+`, UTF-8, a leading/trailing `.`)
// becomes `%XX`, so real names such as "A World of Ice & Fire" or non-Latin
// module names still get their own profile. Returns "" for empty or over-long
// names and for anything containing a path separator, a control byte, or `..`.
[[nodiscard]] inline std::string sanitize_module_profile_id(std::string_view module_name) {
    if (module_name.empty() || module_name.size() > 128U ||
        module_name.find("..") != std::string_view::npos) {
        return {};
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(module_name.size() * 3U);
    for (std::size_t i = 0; i < module_name.size(); ++i) {
        const char c = module_name[i];
        const auto u = static_cast<unsigned char>(c);
        if (c == '/' || c == '\\' || u < 0x20U || u == 0x7fU) {
            return {};
        }
        const bool edge = (i == 0U || i + 1U == module_name.size());
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
            (c == '.' && !edge)) {
            out.push_back(c);
        } else if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back('%');
            out.push_back(kHex[u >> 4U]);
            out.push_back(kHex[u & 0x0fU]);
        }
    }
    return out;
}

[[nodiscard]] inline std::string input_profile_vfs_path(std::string_view module_name) {
    const std::string stem = sanitize_module_profile_id(module_name);
    if (stem.empty()) {
        return {};
    }
    return "input_profiles/" + stem + ".json";
}

[[nodiscard]] inline std::string serialize_input_profile_json(
    const TouchOverlayProfile& profile,
    const WseGameKeyManager* keys = nullptr) {
    std::map<std::string, std::string> kv;
    kv["version"] = std::to_string(profile.version);
    kv["stick_deadzone"] = input_detail::fmt_float(profile.stick_deadzone);
    kv["stick_key_threshold"] = input_detail::fmt_float(profile.stick_key_threshold);
    kv["gamepad_deadzone"] = input_detail::fmt_float(profile.gamepad_deadzone);
    kv["look_sensitivity"] = input_detail::fmt_float(profile.look_sensitivity);
    kv["swipe_threshold"] = input_detail::fmt_float(profile.swipe_threshold);
    kv["radial_deadzone"] = input_detail::fmt_float(profile.radial_deadzone);
    kv["invert_look_y"] = profile.invert_look_y ? "true" : "false";
    kv["touch_overlay_enabled"] = profile.touch_overlay_enabled ? "true" : "false";
    kv["gyro_aim_enabled"] = profile.gyro_aim_enabled ? "true" : "false";
    kv["gyro_sensitivity"] = input_detail::fmt_float(profile.gyro_sensitivity);
    kv["gyro_smoothing"] = input_detail::fmt_float(profile.gyro_smoothing);
    kv["gyro_deadzone"] = input_detail::fmt_float(profile.gyro_deadzone);
    kv["invert_gyro_y"] = profile.invert_gyro_y ? "true" : "false";
    kv["pinch_zoom_enabled"] = profile.pinch_zoom_enabled ? "true" : "false";
    kv["pinch_zoom_sensitivity"] = input_detail::fmt_float(profile.pinch_zoom_sensitivity);
    kv["default_attack_dir"] =
        std::to_string(static_cast<std::int32_t>(profile.default_attack_dir));
    kv["default_block_dir"] =
        std::to_string(static_cast<std::int32_t>(profile.default_block_dir));

    for (std::size_t i = 0; i < kNumTouchWidgets; ++i) {
        const auto id = static_cast<TouchWidgetId>(i);
        const TouchWidgetLayout& w = profile.widgets[i];
        const std::string prefix = std::string("w_") + touch_widget_key(id) + "_";
        kv[prefix + "cx"] = input_detail::fmt_float(w.cx);
        kv[prefix + "cy"] = input_detail::fmt_float(w.cy);
        kv[prefix + "hw"] = input_detail::fmt_float(w.half_w);
        kv[prefix + "hh"] = input_detail::fmt_float(w.half_h);
        kv[prefix + "scale"] = input_detail::fmt_float(w.scale);
        kv[prefix + "opacity"] = input_detail::fmt_float(w.opacity);
        kv[prefix + "enabled"] = w.enabled ? "true" : "false";
    }

    if (keys != nullptr) {
        const auto defaults = default_game_key_bindings();
        for (std::size_t i = 0; i < kNumGameInputKeys; ++i) {
            const GameKeyBinding& b = keys->bindings()[i];
            const GameKeyBinding& d = defaults[i];
            if (b.primary != d.primary || b.alt != d.alt) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%d,%d,%d,%d|%d,%d,%d,%d",
                              b.primary.key, b.primary.num_modifiers,
                              b.primary.modifiers[0], b.primary.modifiers[1],
                              b.alt.key, b.alt.num_modifiers,
                              b.alt.modifiers[0], b.alt.modifiers[1]);
                kv["gk_" + std::to_string(i)] = buf;
            }
        }
    }

    return wse::core::wse_serialize_json_object(kv, true);
}

[[nodiscard]] inline bool deserialize_input_profile_json(
    std::string_view json_text,
    TouchOverlayProfile& out_profile,
    WseGameKeyManager* out_keys = nullptr,
    std::string* error_out = nullptr) {
    const auto fail = [&](std::string_view msg) {
        if (error_out != nullptr) {
            *error_out = std::string(msg);
        }
        return false;
    };

    std::map<std::string, std::string> kv;
    if (!wse::core::wse_parse_json_object(json_text, kv, false)) {
        return fail("corrupt input profile JSON");
    }
    const auto vit = kv.find("version");
    std::int32_t ver = 0;
    if (vit == kv.end() || !input_detail::parse_int_exact(vit->second, ver) || ver != 1) {
        return fail("unsupported or missing input profile version");
    }

    TouchOverlayProfile parsed{};
    auto read_f = [&](const char* key, float& dst) -> bool {
        const auto it = kv.find(key);
        return (it == kv.end()) || input_detail::parse_float_exact(it->second, dst);
    };
    auto read_b = [&](const char* key, bool& dst) -> bool {
        const auto it = kv.find(key);
        return (it == kv.end()) || input_detail::parse_bool_exact(it->second, dst);
    };
    auto read_dir = [&](const char* key, TouchCombatDirection& dst) -> bool {
        const auto it = kv.find(key);
        if (it == kv.end()) {
            return true;
        }
        std::int32_t val = 0;
        if (!input_detail::parse_int_exact(it->second, val) || val < 1 || val > 4) {
            return false;
        }
        dst = static_cast<TouchCombatDirection>(val);
        return true;
    };

    if (!read_f("stick_deadzone", parsed.stick_deadzone) ||
        !read_f("stick_key_threshold", parsed.stick_key_threshold) ||
        !read_f("gamepad_deadzone", parsed.gamepad_deadzone) ||
        !read_f("look_sensitivity", parsed.look_sensitivity) ||
        !read_f("swipe_threshold", parsed.swipe_threshold) ||
        !read_f("radial_deadzone", parsed.radial_deadzone) ||
        !read_b("invert_look_y", parsed.invert_look_y) ||
        !read_b("touch_overlay_enabled", parsed.touch_overlay_enabled) ||
        !read_b("gyro_aim_enabled", parsed.gyro_aim_enabled) ||
        !read_f("gyro_sensitivity", parsed.gyro_sensitivity) ||
        !read_f("gyro_smoothing", parsed.gyro_smoothing) ||
        !read_f("gyro_deadzone", parsed.gyro_deadzone) ||
        !read_b("invert_gyro_y", parsed.invert_gyro_y) ||
        !read_b("pinch_zoom_enabled", parsed.pinch_zoom_enabled) ||
        !read_f("pinch_zoom_sensitivity", parsed.pinch_zoom_sensitivity) ||
        !read_dir("default_attack_dir", parsed.default_attack_dir) ||
        !read_dir("default_block_dir", parsed.default_block_dir)) {
        return fail("invalid scalar in input profile JSON");
    }

    for (std::size_t i = 0; i < kNumTouchWidgets; ++i) {
        const auto id = static_cast<TouchWidgetId>(i);
        TouchWidgetLayout& w = parsed.widgets[i];
        const std::string p = std::string("w_") + touch_widget_key(id) + "_";
        if (!read_f((p + "cx").c_str(), w.cx) || !read_f((p + "cy").c_str(), w.cy) ||
            !read_f((p + "hw").c_str(), w.half_w) || !read_f((p + "hh").c_str(), w.half_h) ||
            !read_f((p + "scale").c_str(), w.scale) ||
            !read_f((p + "opacity").c_str(), w.opacity) ||
            !read_b((p + "enabled").c_str(), w.enabled)) {
            return fail("invalid widget layout field in input profile JSON");
        }
    }
    sanitize_input_profile(parsed);

    if (out_keys != nullptr) {
        WseGameKeyManager parsed_keys;
        parsed_keys.reset();
        auto parse_chord = [](std::string_view sv, KeyChord& out_c) -> bool {
            std::array<std::int32_t, 4> vals{};
            std::size_t start = 0;
            for (std::size_t field = 0; field < 4U; ++field) {
                const std::size_t comma = sv.find(',', start);
                const std::string_view part =
                    (comma == std::string_view::npos) ? sv.substr(start)
                                                      : sv.substr(start, comma - start);
                if (!input_detail::parse_int_exact(part, vals[field])) {
                    return false;
                }
                if (field < 3U && comma == std::string_view::npos) {
                    return false;
                }
                if (field == 3U && comma != std::string_view::npos) {
                    return false;
                }
                start = (comma == std::string_view::npos) ? sv.size() : (comma + 1U);
            }
            out_c.key = vals[0];
            out_c.num_modifiers = vals[1];
            out_c.modifiers[0] = vals[2];
            out_c.modifiers[1] = vals[3];
            return true;
        };

        for (const auto& [k, v] : kv) {
            if (!k.starts_with("gk_")) {
                continue;
            }
            std::int32_t gk = -1;
            if (!input_detail::parse_int_exact(std::string_view(k).substr(3U), gk) || gk < 0 ||
                gk >= kNumGameInputKeys) {
                return fail("invalid game key index in input profile JSON");
            }
            const std::size_t sep = v.find('|');
            if (sep == std::string::npos) {
                return fail("invalid game key chord encoding");
            }
            KeyChord pri{};
            KeyChord alt{};
            if (!parse_chord(std::string_view(v).substr(0U, sep), pri) ||
                !parse_chord(std::string_view(v).substr(sep + 1U), alt)) {
                return fail("malformed game key chord numbers");
            }
            (void)parsed_keys.set_key(gk, 0, pri);
            (void)parsed_keys.set_key(gk, 1, alt);
        }
        *out_keys = std::move(parsed_keys);
    }

    out_profile = parsed;
    return true;
}

[[nodiscard]] inline bool save_input_profile(Vfs& vfs,
                                             std::string_view module_name,
                                             const TouchOverlayProfile& profile,
                                             const WseGameKeyManager* keys = nullptr,
                                             std::string* error_out = nullptr) {
    const std::string path = input_profile_vfs_path(module_name);
    if (path.empty()) {
        if (error_out != nullptr) {
            *error_out = "invalid module profile name";
        }
        return false;
    }
    const std::string json = serialize_input_profile_json(profile, keys);
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
    // `input_profiles/` does not exist on a fresh install; a failure surfaces at the write.
    (void)make_parent_dirs(vfs, path);
    const std::string tmp = path + ".partial";
    if (!vfs.write_file(tmp, bytes)) {
        if (error_out != nullptr) {
            *error_out = "failed writing input profile";
        }
        return false;
    }
    if (!vfs.rename_file(tmp, path)) {
        (void)vfs.remove_file(tmp);
        if (!vfs.write_file(path, bytes)) {
            if (error_out != nullptr) {
                *error_out = "failed committing input profile";
            }
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool load_input_profile(Vfs& vfs,
                                             std::string_view module_name,
                                             TouchOverlayProfile& out_profile,
                                             WseGameKeyManager* out_keys = nullptr,
                                             std::string* error_out = nullptr) {
    const std::string path = input_profile_vfs_path(module_name);
    if (path.empty()) {
        if (error_out != nullptr) {
            *error_out = "invalid module profile name";
        }
        return false;
    }
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(path, raw)) {
        if (error_out != nullptr) {
            *error_out = "input profile not found";
        }
        return false;
    }
    const std::string_view sv(reinterpret_cast<const char*>(raw.data()), raw.size());
    return deserialize_input_profile_json(sv, out_profile, out_keys, error_out);
}

// Unified device event stream produced by SDL3 (`platform/sdl/sdl_input.cpp`)
// or by `SyntheticInputBackend` in headless host tests.
// Rotation of the drawn image relative to the device's natural orientation (Android
// `Surface.ROTATION_*`; SDL reports it as `SDL_GetCurrentDisplayOrientation` minus
// `SDL_GetNaturalDisplayOrientation`).
enum class ScreenRotation : std::uint8_t {
    Deg0 = 0,
    Deg90 = 1,
    Deg180 = 2,
    Deg270 = 3,
};

[[nodiscard]] constexpr ScreenRotation screen_rotation_from_degrees(int degrees) noexcept {
    const int d = ((degrees % 360) + 360) % 360;
    if (d >= 45 && d < 135) {
        return ScreenRotation::Deg90;
    }
    if (d >= 135 && d < 225) {
        return ScreenRotation::Deg180;
    }
    return (d >= 225 && d < 315) ? ScreenRotation::Deg270 : ScreenRotation::Deg0;
}

struct GyroLookRates {
    float yaw_right = 0.0f;  // rad/s, + turns right
    float pitch_up = 0.0f;   // rad/s, + looks up
};

// Converts a phone/tablet gyroscope sample (rad/s, counter-clockwise positive, axes in
// the device's natural orientation: +X right, +Y top, +Z toward the viewer; sensor
// APIs do not rotate them with the screen) into look rates for the current screen
// rotation. With screen axes from `SensorManager.remapCoordinateSystem` (ROTATION_90:
// screen X = +Y, screen Y = -X), yaw is the rotation about screen-up and pitch about
// screen-right. Game controllers report in their own frame: use `Deg0`.
[[nodiscard]] constexpr GyroLookRates gyro_look_rates(ScreenRotation rotation,
                                                      float gx,
                                                      float gy) noexcept {
    switch (rotation) {
    case ScreenRotation::Deg90:
        return {gx, gy};
    case ScreenRotation::Deg180:
        return {gy, -gx};
    case ScreenRotation::Deg270:
        return {-gx, -gy};
    case ScreenRotation::Deg0:
    default:
        return {-gy, gx};
    }
}

struct RawInputEvent {
    enum class Type : std::uint8_t {
        KeyDown = 0,
        KeyUp = 1,
        MouseButtonDown = 2,
        MouseButtonUp = 3,
        MouseWheel = 4,
        MouseMotion = 5,
        GamepadConnected = 6,
        GamepadDisconnected = 7,
        GamepadButtonDown = 8,
        GamepadButtonUp = 9,
        GamepadAxis = 10,
        TouchDown = 11,
        TouchMove = 12,
        TouchUp = 13,
        // Window lost focus / app went to the background: release everything held.
        FocusLost = 14,
        // The OS took the touch over (system gesture, palm rejection): drop the
        // finger without committing it (no radial order fires).
        TouchCancel = 15,
        // Angular velocity sample from a gyroscope (`dx` = yaw rate rad/s
        // +right, `dy` = pitch rate rad/s +up). Used only in `Mission` while
        // `gyro_aim_enabled` is true.
        GyroSample = 16,
    };

    Type type = Type::KeyDown;
    std::int32_t raw_key = -1;
    std::uint64_t device_id = 0;
    std::int64_t finger_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float left_x = 0.0f;
    float left_y = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;
    float left_trigger = 0.0f;
    float right_trigger = 0.0f;
    bool is_repeat = false;
};

class InputBackend {
public:
    virtual ~InputBackend() = default;
    virtual void poll_events(std::vector<RawInputEvent>& out_events) = 0;
    [[nodiscard]] virtual const char* backend_name() const noexcept = 0;
};

class NullInputBackend final : public InputBackend {
public:
    void poll_events(std::vector<RawInputEvent>& out_events) override { out_events.clear(); }
    [[nodiscard]] const char* backend_name() const noexcept override { return "null"; }
};

class SyntheticInputBackend final : public InputBackend {
public:
    void push(RawInputEvent ev) { queue_.push_back(ev); }
    void push_batch(std::span<const RawInputEvent> events) {
        queue_.insert(queue_.end(), events.begin(), events.end());
    }
    void poll_events(std::vector<RawInputEvent>& out_events) override {
        out_events.swap(queue_);
        queue_.clear();
    }
    [[nodiscard]] const char* backend_name() const noexcept override { return "synthetic"; }

private:
    std::vector<RawInputEvent> queue_{};
};

// Per-frame synthesized input state combining physical devices and touch overlay.
struct InputFrameState {
    InputContext context = InputContext::Mission;
    bool gamepad_connected = false;
    bool touch_overlay_visible = true;
    float cursor_x = 0.5f;
    float cursor_y = 0.5f;
    float move_x = 0.0f;  // [-1, 1], +X right
    float move_y = 0.0f;  // [-1, 1], +Y forward
    float look_yaw_delta = 0.0f;
    float look_pitch_delta = 0.0f;
    TouchCombatDirection attack_dir = TouchCombatDirection::None;
    bool attack_active = false;
    TouchCombatDirection block_dir = TouchCombatDirection::None;
    bool block_active = false;
    bool radial_open = false;
    std::int32_t radial_level = 0;    // 0 = closed, 1 = group ring, 2 = order ring
    std::int32_t radial_group = -1;   // 0..3 (All/Infantry/Archers/Cavalry), else -1
    std::int32_t radial_sector = -1;  // 0..7 when hovered in level 2, else -1
    float stick_knob_x = 0.0f;
    float stick_knob_y = 0.0f;
    // CampaignMap 2-finger pinch zoom delta (>0 zooms in, <0 zooms out).
    float map_zoom_delta = 0.0f;
    bool pinch_active = false;
    bool map_follow = true;
    bool map_pan_mode = false;
    bool map_paused = false;
    std::array<bool, kNumTouchWidgets> widget_pressed{};
};

// Maps physical devices (`RawInputEvent`) + virtual touch overlay + `InputContext`
// onto `WseGameKeyManager` states (`game_key_down`/`clicked`/`released`) and `InputFrameState`.
//
// Edge semantics:
//   - A raw key "click" / "release" is latched when the transition happens, so a
//     key pressed and released inside one frame still reports a click (and a
//     release) instead of vanishing. Each wheel notch is its own click.
//   - Physical game-key clicks / releases follow `mbGameKeyManager::keyClicked`
//     (`0x1400eeff0`) / `keyRaised` (`0x1400ef0b0`): the chord's modifiers must be
//     held and the *main* key must change state, so holding J and then pressing
//     Ctrl does not click `RearHorse`.
//   - `omit_key_once(raw)` suppresses the next press of that key for game-key
//     purposes (until it is released) and is then consumed.
class WseInputManager {
public:
    static constexpr float kGamepadTriggerThreshold = 0.35f;
    static constexpr std::size_t kMaxTrackedFingers = 10U;
    // `map_zoom_delta` is the natural log of the finger-spread ratio (times
    // `pinch_zoom_sensitivity`), so the camera follows the fingers when it scales its
    // distance by `exp(-map_zoom_delta)`, and the same gesture zooms equally whether it
    // starts close together or wide apart, on a phone or a tablet.
    static constexpr float kPinchZoomScale = 1.0f;
    // Finger spreads below this (screen-height units) are clamped, so two touches
    // landing on almost the same point cannot produce a huge ratio.
    static constexpr float kMinPinchDistance = 0.02f;
    // A frame with no gyro sample reuses the last rate for this long.
    static constexpr float kGyroSampleHoldSeconds = 0.05f;
    // Attack / block swipe classification waits until displacement exceeds
    // `swipe_threshold` or this hold duration elapses (whichever comes first).
    static constexpr float kSwipeClassifyDelaySeconds = 0.120f;
    // Gamepad raw keys occupy `0xf0..0xff` in `rglInputManager::getKeyName` (`0x1402ad5d0`).
    static constexpr std::int32_t kFirstGamepadRawKey = RawKey::kGamepadA;
    static constexpr std::size_t kNumGamepadRawKeys = 16U;

    WseInputManager() { reset(); }

    void reset() {
        key_mgr_.reset();
        profile_ = {};
        editor_ = {};
        context_ = InputContext::Mission;
        raw_down_.fill(false);
        raw_prev_down_.fill(false);
        raw_clicked_.fill(false);
        raw_released_.fill(false);
        omitted_keys_.fill(false);
        suppressed_keys_.fill(false);
        game_down_.fill(false);
        game_prev_down_.fill(false);
        game_clicked_.fill(false);
        game_released_.fill(false);
        virtual_held_.fill(false);
        virtual_prev_held_.fill(false);
        virtual_pulse_gk_.fill(false);
        pulse_prev_.fill(false);
        gamepads_.clear();
        gamepad_active_ = false;
        fingers_.clear();
        stick_x_ = 0.0f;
        stick_y_ = 0.0f;
        attack_dir_ = TouchCombatDirection::None;
        block_dir_ = TouchCombatDirection::None;
        attack_pulse_dir_ = false;
        block_pulse_dir_ = false;
        radial_open_ = false;
        radial_level_ = 0;
        radial_group_ = -1;
        radial_sector_ = -1;
        pinch_active_ = false;
        pinch_prev_dist_ = 0.0f;
        pinch_id0_ = -1;
        pinch_id1_ = -1;
        map_follow_ = true;
        map_pan_mode_ = false;
        map_paused_ = false;
        gyro_filt_yaw_ = 0.0f;
        gyro_filt_pitch_ = 0.0f;
        cursor_x_ = 0.5f;
        cursor_y_ = 0.5f;
        frame_ = {};
        frame_.touch_overlay_visible = true;
    }

    [[nodiscard]] WseGameKeyManager& game_keys() noexcept { return key_mgr_; }
    [[nodiscard]] const WseGameKeyManager& game_keys() const noexcept { return key_mgr_; }

    [[nodiscard]] TouchOverlayProfile& profile() noexcept { return profile_; }
    [[nodiscard]] const TouchOverlayProfile& profile() const noexcept { return profile_; }

    [[nodiscard]] TouchLayoutEditor& layout_editor() noexcept { return editor_; }
    [[nodiscard]] const TouchLayoutEditor& layout_editor() const noexcept { return editor_; }

    void set_context(InputContext ctx) noexcept {
        if (context_ != ctx) {
            context_ = ctx;
            if (ctx != InputContext::Mission) {
                reset_gyro_filter();
            }
            if (ctx != InputContext::CampaignMap) {
                pinch_active_ = false;
                pinch_prev_dist_ = 0.0f;
                pinch_id0_ = -1;
                pinch_id1_ = -1;
            }
            if (!is_touch_overlay_visible()) {
                clear_touch_state();
            } else {
                prune_inactive_touch_state();
                update_pinch_state();
            }
        }
    }
    [[nodiscard]] InputContext context() const noexcept { return context_; }

    // Viewport size in the same units the backend normalizes touch/mouse with.
    void set_viewport_size(int width, int height) noexcept {
        viewport_aspect_ = (width > 0 && height > 0)
                               ? sanitize_viewport_aspect(static_cast<float>(width) /
                                                          static_cast<float>(height))
                               : 1.0f;
    }
    [[nodiscard]] float viewport_aspect() const noexcept { return viewport_aspect_; }

    // Mouse motion turns the camera only while this is set (typically: relative /
    // captured mouse mode) and only in `Mission` / `CampaignMap`, so moving the
    // cursor over menus or the inventory never spins the view.
    void set_mouse_look_enabled(bool enabled) noexcept { mouse_look_enabled_ = enabled; }
    [[nodiscard]] bool mouse_look_enabled() const noexcept { return mouse_look_enabled_; }

    [[nodiscard]] bool is_gamepad_connected() const noexcept { return !gamepads_.empty(); }

    // The overlay hides while a connected gamepad is the device in use; touching the
    // screen brings it back (until the gamepad is used again). Devices that always
    // report a controller (TV boxes, some phones) therefore keep touch controls.
    [[nodiscard]] bool is_touch_overlay_visible() const noexcept {
        return profile_.touch_overlay_enabled && !(is_gamepad_connected() && gamepad_active_) &&
               context_allows_touch_overlay(context_);
    }

    // Whether a finger landing at (`x`, `y`) (viewport-normalized, `y` down) would be taken
    // by the visible overlay in this context, as `handle_touch_down` decides it: a widget hit,
    // or any touch on the campaign map while pinch zoom is on. Hosts use it to keep overlay
    // touches away from the menu UI.
    [[nodiscard]] bool overlay_claims_touch(float x, float y) const noexcept {
        if (!is_touch_overlay_visible() || !std::isfinite(x) || !std::isfinite(y)) {
            return false;
        }
        return pick_widget_for_touch(x, y) != TouchWidgetId::Count ||
               (context_ == InputContext::CampaignMap && profile_.pinch_zoom_enabled);
    }

    [[nodiscard]] bool is_widget_visible(TouchWidgetId id) const noexcept {
        if (id >= TouchWidgetId::Count) {
            return false;
        }
        return is_touch_widget_active(id, context_, profile_.widgets[static_cast<std::size_t>(id)],
                                      is_touch_overlay_visible());
    }

    // `omit_key_once` (opcode `77`): the next press of `raw_key` is ignored by game-key
    // mapping until that key is released; the request is then consumed.
    void omit_key_once(std::int32_t raw_key) noexcept {
        if (raw_key >= 0 && raw_key < kMaxRawInputKeys) {
            omitted_keys_[static_cast<std::size_t>(raw_key)] = true;
        }
    }
    void clear_omitted_keys() noexcept { omitted_keys_.fill(false); }
    void set_omitted_keys(const std::array<bool, kMaxRawInputKeys>& keys) noexcept {
        omitted_keys_ = keys;
    }
    [[nodiscard]] const std::array<bool, kMaxRawInputKeys>& omitted_keys() const noexcept {
        return omitted_keys_;
    }

    // Advances one frame using events queued in `backend`.
    void step(InputBackend& backend, float dt_seconds = 1.0f / 60.0f) {
        poll_scratch_.clear();
        backend.poll_events(poll_scratch_);
        process_frame(poll_scratch_, dt_seconds);
    }

    // Advances one frame from a batch of `RawInputEvent`.
    void process_frame(std::span<const RawInputEvent> events, float dt_seconds = 1.0f / 60.0f) {
        const float safe_dt =
            (std::isfinite(dt_seconds) && dt_seconds > 0.0f) ? std::min(dt_seconds, 0.25f) : 0.0f;
        const float aspect = viewport_aspect_;

        raw_prev_down_ = raw_down_;
        game_prev_down_ = game_down_;
        virtual_prev_held_ = virtual_held_;
        pulse_prev_ = virtual_pulse_gk_;
        raw_clicked_.fill(false);
        raw_released_.fill(false);
        virtual_pulse_gk_.fill(false);
        // Wheel notches are one-frame impulses.
        release_raw(RawKey::kScrollUp);
        release_raw(RawKey::kScrollDown);

        ++frame_counter_;
        attack_pulse_dir_ = false;
        block_pulse_dir_ = false;
        if (!has_finger_for(TouchWidgetId::AttackZone)) {
            attack_dir_ = TouchCombatDirection::None;
        }
        if (!has_finger_for(TouchWidgetId::BlockZone)) {
            block_dir_ = TouchCombatDirection::None;
        }
        for (auto& f : fingers_) {
            f.elapsed_seconds += safe_dt;
        }
        float look_dx = 0.0f;  // screen-height units
        float look_dy = 0.0f;
        float map_zoom_delta = 0.0f;
        float gyro_sum_yaw = 0.0f;
        float gyro_sum_pitch = 0.0f;
        int gyro_samples = 0;

        if (!profile_.pinch_zoom_enabled || context_ != InputContext::CampaignMap) {
            pinch_active_ = false;
            pinch_prev_dist_ = 0.0f;
            pinch_id0_ = -1;
            pinch_id1_ = -1;
        }

        for (const RawInputEvent& ev : events) {
            switch (ev.type) {
            case RawInputEvent::Type::KeyDown:
            case RawInputEvent::Type::MouseButtonDown:
                press_raw(ev.raw_key);
                break;
            case RawInputEvent::Type::KeyUp:
            case RawInputEvent::Type::MouseButtonUp:
                release_raw(ev.raw_key);
                break;
            case RawInputEvent::Type::GamepadButtonDown:
            case RawInputEvent::Type::GamepadButtonUp: {
                const bool down = (ev.type == RawInputEvent::Type::GamepadButtonDown);
                GamepadState& pad = pad_for(ev.device_id);
                const std::int32_t slot = ev.raw_key - kFirstGamepadRawKey;
                if (slot >= 0 && static_cast<std::size_t>(slot) < kNumGamepadRawKeys) {
                    pad.buttons[static_cast<std::size_t>(slot)] = down;
                    refresh_gamepad_key(ev.raw_key);
                } else if (down) {
                    press_raw(ev.raw_key);
                } else {
                    release_raw(ev.raw_key);
                }
                if (down) {
                    mark_gamepad_used();
                }
                break;
            }
            case RawInputEvent::Type::MouseWheel:
                if (ev.dy > 0.0f) {
                    press_raw(RawKey::kScrollUp);
                } else if (ev.dy < 0.0f) {
                    press_raw(RawKey::kScrollDown);
                }
                break;
            case RawInputEvent::Type::MouseMotion:
                if (std::isfinite(ev.x) && std::isfinite(ev.y)) {
                    cursor_x_ = std::clamp(ev.x, 0.0f, 1.0f);
                    cursor_y_ = std::clamp(ev.y, 0.0f, 1.0f);
                }
                if (mouse_look_enabled_ && context_allows_touch_overlay(context_) &&
                    std::isfinite(ev.dx) && std::isfinite(ev.dy)) {
                    look_dx += ev.dx * aspect;
                    look_dy += ev.dy;
                }
                break;
            case RawInputEvent::Type::GamepadConnected:
                (void)pad_for(ev.device_id);
                mark_gamepad_used();
                break;
            case RawInputEvent::Type::GamepadDisconnected:
                remove_gamepad(ev.device_id);
                break;
            case RawInputEvent::Type::GamepadAxis: {
                GamepadState& pad = pad_for(ev.device_id);
                const auto set_axis = [](float v, float lo, float& dst) {
                    if (std::isfinite(v)) {
                        dst = std::clamp(v, lo, 1.0f);
                    }
                };
                set_axis(ev.left_x, -1.0f, pad.lx);
                set_axis(ev.left_y, -1.0f, pad.ly);
                set_axis(ev.right_x, -1.0f, pad.rx);
                set_axis(ev.right_y, -1.0f, pad.ry);
                set_axis(ev.left_trigger, 0.0f, pad.lt);
                set_axis(ev.right_trigger, 0.0f, pad.rt);
                refresh_trigger_keys();
                const float dz = profile_.gamepad_deadzone;
                if (std::hypot(pad.lx, pad.ly) > dz || std::hypot(pad.rx, pad.ry) > dz ||
                    pad.lt >= kGamepadTriggerThreshold || pad.rt >= kGamepadTriggerThreshold) {
                    mark_gamepad_used();
                }
                break;
            }
            case RawInputEvent::Type::TouchDown:
                if (std::isfinite(ev.x) && std::isfinite(ev.y)) {
                    cursor_x_ = std::clamp(ev.x, 0.0f, 1.0f);
                    cursor_y_ = std::clamp(ev.y, 0.0f, 1.0f);
                    gamepad_active_ = false;  // touching the screen re-shows the overlay
                    if (is_touch_overlay_visible()) {
                        handle_touch_down(ev.finger_id, cursor_x_, cursor_y_);
                    }
                }
                break;
            case RawInputEvent::Type::TouchMove:
                if (std::isfinite(ev.x) && std::isfinite(ev.y)) {
                    cursor_x_ = std::clamp(ev.x, 0.0f, 1.0f);
                    cursor_y_ = std::clamp(ev.y, 0.0f, 1.0f);
                    if (is_touch_overlay_visible()) {
                        handle_touch_move(ev.finger_id, cursor_x_, cursor_y_, look_dx, look_dy,
                                          map_zoom_delta);
                    }
                }
                break;
            case RawInputEvent::Type::TouchUp:
                handle_touch_up(ev.finger_id, true);
                break;
            case RawInputEvent::Type::TouchCancel:
                handle_touch_up(ev.finger_id, false);
                break;
            case RawInputEvent::Type::FocusLost:
                release_everything();
                gyro_sum_yaw = 0.0f;
                gyro_sum_pitch = 0.0f;
                gyro_samples = 0;
                break;
            case RawInputEvent::Type::GyroSample:
                if (profile_.gyro_aim_enabled && context_ == InputContext::Mission &&
                    std::isfinite(ev.dx) && std::isfinite(ev.dy)) {
                    gyro_sum_yaw += ev.dx;
                    gyro_sum_pitch += ev.dy;
                    ++gyro_samples;
                }
                break;
            }
        }

        if (!is_touch_overlay_visible()) {
            clear_touch_state();
        } else {
            resolve_delayed_swipes(aspect);
        }

        // `omit_key_once`: a fresh press of an omitted key is swallowed (until release).
        for (std::size_t k = 0; k < static_cast<std::size_t>(kMaxRawInputKeys); ++k) {
            // Same-frame release+repress ends the swallowed hold before consuming any new omit.
            if (suppressed_keys_[k] && raw_released_[k] && raw_down_[k]) {
                suppressed_keys_[k] = false;
            }
            if (raw_clicked_[k] && omitted_keys_[k]) {
                omitted_keys_[k] = false;
                suppressed_keys_[k] = true;
            }
        }

        // Key views for chord matching with suppressed keys masked out.
        std::array<bool, kMaxRawInputKeys> down_now{};
        std::array<bool, kMaxRawInputKeys> seen_down{};
        std::array<bool, kMaxRawInputKeys> prev_for_click{};
        std::array<bool, kMaxRawInputKeys> prev_for_raise{};
        for (std::size_t k = 0; k < static_cast<std::size_t>(kMaxRawInputKeys); ++k) {
            const bool live = !suppressed_keys_[k];
            down_now[k] = live && raw_down_[k];
            seen_down[k] = live && (raw_down_[k] || raw_clicked_[k]);
            prev_for_click[k] = live && raw_prev_down_[k] && !raw_clicked_[k];
            prev_for_raise[k] = live && (raw_prev_down_[k] || raw_clicked_[k]);
        }

        virtual_held_.fill(false);
        synthesize_overlay_game_keys(virtual_held_);

        for (std::size_t g = 0; g < static_cast<std::size_t>(kNumGameInputKeys); ++g) {
            const std::int32_t gid = static_cast<std::int32_t>(g);
            const bool phys_down = key_mgr_.key_down(gid, down_now);
            const bool phys_click = key_mgr_.key_clicked(gid, seen_down, prev_for_click);
            const bool phys_raise = key_mgr_.key_raised(gid, down_now, prev_for_raise);
            const bool virt_click = virtual_held_[g] && !virtual_prev_held_[g];
            const bool virt_raise = !virtual_held_[g] && virtual_prev_held_[g];
            const bool pulse_raise = pulse_prev_[g] && !virtual_pulse_gk_[g];
            const bool d = phys_down || virtual_held_[g] || virtual_pulse_gk_[g];
            game_down_[g] = d;
            game_clicked_[g] =
                phys_click || ((virt_click || virtual_pulse_gk_[g]) && !game_prev_down_[g]);
            game_released_[g] = !d && (phys_raise || virt_raise || pulse_raise);
        }

        for (std::size_t k = 0; k < static_cast<std::size_t>(kMaxRawInputKeys); ++k) {
            if (!raw_down_[k]) {
                suppressed_keys_[k] = false;
            }
        }

        // Build consolidated analog + look + combat frame state.
        frame_.context = context_;
        frame_.gamepad_connected = is_gamepad_connected();
        frame_.touch_overlay_visible = is_touch_overlay_visible();
        frame_.cursor_x = cursor_x_;
        frame_.cursor_y = cursor_y_;

        float glx = 0.0f;
        float gly = 0.0f;
        float grx = 0.0f;
        float gry = 0.0f;
        gamepad_sticks(glx, gly, grx, gry);

        // Analog sources add directly; only *physical* move keys add a full unit
        // (the keys the overlay / gamepad synthesize must not double-count).
        float mx = stick_x_ + glx;
        float my = stick_y_ + gly;
        const auto phys = [&](GameKeyId id) {
            return key_mgr_.key_down(static_cast<std::int32_t>(id), down_now);
        };
        mx += (phys(GameKeyId::MoveRight) ? 1.0f : 0.0f) - (phys(GameKeyId::MoveLeft) ? 1.0f : 0.0f);
        my += (phys(GameKeyId::MoveForward) ? 1.0f : 0.0f) -
              (phys(GameKeyId::MoveBackward) ? 1.0f : 0.0f);
        const float mag = std::sqrt(mx * mx + my * my);
        if (mag > 1.0f) {  // diagonals are not faster than straight lines
            mx /= mag;
            my /= mag;
        }
        frame_.move_x = mx;
        frame_.move_y = my;

        float gyro_yaw_delta = 0.0f;
        float gyro_pitch_delta = 0.0f;
        if (!profile_.gyro_aim_enabled || context_ != InputContext::Mission) {
            reset_gyro_filter();
        } else {
            // Sensors often report slower than the display refreshes (50 Hz gyro at a
            // 90/120 Hz cap): a frame without a sample keeps the last rate for up to
            // `kGyroSampleHoldSeconds` instead of reading as "not turning", which made the
            // aim stutter every other frame.
            if (gyro_samples > 0) {
                gyro_hold_yaw_ = gyro_sum_yaw / static_cast<float>(gyro_samples);
                gyro_hold_pitch_ = gyro_sum_pitch / static_cast<float>(gyro_samples);
                gyro_hold_age_ = 0.0f;
            } else {
                gyro_hold_age_ += safe_dt;
                if (gyro_hold_age_ > kGyroSampleHoldSeconds) {
                    gyro_hold_yaw_ = 0.0f;
                    gyro_hold_pitch_ = 0.0f;
                }
            }
            const float target_yaw = gyro_hold_yaw_;
            const float target_pitch = gyro_hold_pitch_;
            // `gyro_smoothing` is the per-frame factor at 60 Hz; scaling the exponent by
            // the frame time keeps the same time constant at 30/90/120 Hz (and when
            // thermal throttling changes the cap mid-mission).
            const float alpha60 = std::clamp(profile_.gyro_smoothing, 0.0f, 0.95f);
            const float alpha =
                (alpha60 > 0.0f) ? std::pow(alpha60, safe_dt * 60.0f) : 0.0f;
            gyro_filt_yaw_ = alpha * gyro_filt_yaw_ + (1.0f - alpha) * target_yaw;
            gyro_filt_pitch_ = alpha * gyro_filt_pitch_ + (1.0f - alpha) * target_pitch;
            if (target_yaw == 0.0f && target_pitch == 0.0f) {
                if (std::fabs(gyro_filt_yaw_) < 1e-6f) {
                    gyro_filt_yaw_ = 0.0f;
                }
                if (std::fabs(gyro_filt_pitch_) < 1e-6f) {
                    gyro_filt_pitch_ = 0.0f;
                }
            }
            const float filt_mag = std::hypot(gyro_filt_yaw_, gyro_filt_pitch_);
            const float dz = profile_.gyro_deadzone;
            if (filt_mag > dz && filt_mag > 0.0f && safe_dt > 0.0f) {
                const float mag_scale = (dz > 0.0f) ? ((filt_mag - dz) / filt_mag) : 1.0f;
                const float gyro_y_sign = profile_.invert_gyro_y ? -1.0f : 1.0f;
                gyro_yaw_delta =
                    gyro_filt_yaw_ * mag_scale * profile_.gyro_sensitivity * safe_dt;
                gyro_pitch_delta =
                    gyro_filt_pitch_ * mag_scale * profile_.gyro_sensitivity * safe_dt * gyro_y_sign;
            }
        }

        const float y_sign = profile_.invert_look_y ? -1.0f : 1.0f;
        // Dragging right (+look_dx) turns right (+yaw); dragging up (-look_dy) pitches up (+pitch).
        frame_.look_yaw_delta = look_dx * profile_.look_sensitivity +
                                grx * profile_.look_sensitivity * safe_dt + gyro_yaw_delta;
        // `invert_gyro_y` alone flips the gyro (applying `invert_look_y` too made
        // enabling both cancel out).
        frame_.look_pitch_delta = (-look_dy * profile_.look_sensitivity +
                                   gry * profile_.look_sensitivity * safe_dt) *
                                      y_sign +
                                  gyro_pitch_delta;

        frame_.attack_dir = attack_dir_;
        frame_.attack_active =
            has_classified_finger_for(TouchWidgetId::AttackZone) ||
            game_down_[static_cast<std::size_t>(GameKeyId::Attack)];
        frame_.block_dir = block_dir_;
        frame_.block_active =
            has_classified_finger_for(TouchWidgetId::BlockZone) ||
            game_down_[static_cast<std::size_t>(GameKeyId::Defend)];
        frame_.radial_open = radial_open_;
        frame_.radial_level = radial_level_;
        frame_.radial_group = radial_group_;
        frame_.radial_sector = radial_sector_;
        frame_.stick_knob_x = stick_x_;
        frame_.stick_knob_y = stick_y_;
        frame_.map_zoom_delta = map_zoom_delta;
        frame_.pinch_active = pinch_active_;
        frame_.map_follow = map_follow_;
        frame_.map_pan_mode = map_pan_mode_;
        frame_.map_paused = map_paused_;
        for (std::size_t i = 0; i < kNumTouchWidgets; ++i) {
            frame_.widget_pressed[i] = has_finger_for(static_cast<TouchWidgetId>(i));
        }
    }

    [[nodiscard]] const InputFrameState& frame_state() const noexcept { return frame_; }
    // Number of frames processed so far; lets consumers apply one-frame impulses
    // (such as `map_zoom_delta`) exactly once even if they mirror state repeatedly.
    [[nodiscard]] std::uint64_t frame_counter() const noexcept { return frame_counter_; }

    [[nodiscard]] const std::array<bool, kMaxRawInputKeys>& raw_down() const noexcept {
        return raw_down_;
    }
    [[nodiscard]] const std::array<bool, kMaxRawInputKeys>& raw_prev_down() const noexcept {
        return raw_prev_down_;
    }
    // Keys that went down at any point this frame (also set for a press + release
    // inside the frame, and for every wheel notch).
    [[nodiscard]] const std::array<bool, kMaxRawInputKeys>& raw_clicked() const noexcept {
        return raw_clicked_;
    }
    // Keys that went up at any point this frame.
    [[nodiscard]] const std::array<bool, kMaxRawInputKeys>& raw_released() const noexcept {
        return raw_released_;
    }
    [[nodiscard]] const std::array<bool, kNumGameInputKeys>& game_key_down() const noexcept {
        return game_down_;
    }
    [[nodiscard]] const std::array<bool, kNumGameInputKeys>& game_key_prev_down() const noexcept {
        return game_prev_down_;
    }
    [[nodiscard]] const std::array<bool, kNumGameInputKeys>& game_key_clicked() const noexcept {
        return game_clicked_;
    }
    [[nodiscard]] const std::array<bool, kNumGameInputKeys>& game_key_released() const noexcept {
        return game_released_;
    }

private:
    struct ActiveFinger {
        std::int64_t finger_id = 0;
        TouchWidgetId widget = TouchWidgetId::LookArea;
        float anchor_x = 0.0f;
        float anchor_y = 0.0f;
        float last_x = 0.0f;
        float last_y = 0.0f;
        float radial_origin_x = 0.0f;
        float radial_origin_y = 0.0f;
        std::uint64_t down_frame = 0U;
        float elapsed_seconds = 0.0f;
        bool look_enabled = true;
        bool swipe_classified = false;
        TouchCombatDirection swipe_dir = TouchCombatDirection::None;
    };

    struct GamepadState {
        std::uint64_t id = 0;
        float lx = 0.0f;
        float ly = 0.0f;
        float rx = 0.0f;
        float ry = 0.0f;
        float lt = 0.0f;
        float rt = 0.0f;
        std::array<bool, kNumGamepadRawKeys> buttons{};
    };

    void press_raw(std::int32_t raw_key) noexcept {
        if (raw_key < 0 || raw_key >= kMaxRawInputKeys) {
            return;
        }
        const auto k = static_cast<std::size_t>(raw_key);
        if (!raw_down_[k]) {
            raw_clicked_[k] = true;
        }
        raw_down_[k] = true;
    }

    void release_raw(std::int32_t raw_key) noexcept {
        if (raw_key < 0 || raw_key >= kMaxRawInputKeys) {
            return;
        }
        const auto k = static_cast<std::size_t>(raw_key);
        if (raw_down_[k]) {
            raw_released_[k] = true;
        }
        raw_down_[k] = false;
    }

    GamepadState& pad_for(std::uint64_t id) {
        for (auto& p : gamepads_) {
            if (p.id == id) {
                return p;
            }
        }
        gamepads_.push_back(GamepadState{});
        gamepads_.back().id = id;
        return gamepads_.back();
    }

    void remove_gamepad(std::uint64_t id) {
        gamepads_.erase(std::remove_if(gamepads_.begin(), gamepads_.end(),
                                       [id](const GamepadState& p) { return p.id == id; }),
                        gamepads_.end());
        // Buttons the removed pad was holding must not stay stuck down.
        for (std::size_t i = 0; i < kNumGamepadRawKeys; ++i) {
            refresh_gamepad_key(kFirstGamepadRawKey + static_cast<std::int32_t>(i));
        }
        refresh_trigger_keys();
        if (gamepads_.empty()) {
            gamepad_active_ = false;
        }
    }

    void mark_gamepad_used() noexcept {
        if (!gamepad_active_) {
            gamepad_active_ = true;
            clear_touch_state();
        }
    }

    // A gamepad raw key is down while any connected pad holds it (triggers also
    // count past `kGamepadTriggerThreshold`).
    void refresh_gamepad_key(std::int32_t raw_key) noexcept {
        const std::int32_t slot = raw_key - kFirstGamepadRawKey;
        if (slot < 0 || static_cast<std::size_t>(slot) >= kNumGamepadRawKeys) {
            return;
        }
        bool held = false;
        for (const auto& p : gamepads_) {
            held = held || p.buttons[static_cast<std::size_t>(slot)];
            if (raw_key == RawKey::kLeftTrigger) {
                held = held || p.lt >= kGamepadTriggerThreshold;
            } else if (raw_key == RawKey::kRightTrigger) {
                held = held || p.rt >= kGamepadTriggerThreshold;
            }
        }
        if (held) {
            press_raw(raw_key);
        } else {
            release_raw(raw_key);
        }
    }

    void refresh_trigger_keys() noexcept {
        refresh_gamepad_key(RawKey::kLeftTrigger);
        refresh_gamepad_key(RawKey::kRightTrigger);
    }

    // Per stick, the pad deflected furthest wins (an idle second pad cannot zero
    // the stick of the pad being used). Deadzone applied.
    void gamepad_sticks(float& lx, float& ly, float& rx, float& ry) const noexcept {
        float best_l = -1.0f;
        float best_r = -1.0f;
        float raw_lx = 0.0f;
        float raw_ly = 0.0f;
        float raw_rx = 0.0f;
        float raw_ry = 0.0f;
        for (const auto& p : gamepads_) {
            const float ml = std::hypot(p.lx, p.ly);
            if (ml > best_l) {
                best_l = ml;
                raw_lx = p.lx;
                raw_ly = p.ly;
            }
            const float mr = std::hypot(p.rx, p.ry);
            if (mr > best_r) {
                best_r = mr;
                raw_rx = p.rx;
                raw_ry = p.ry;
            }
        }
        apply_radial_deadzone(raw_lx, -raw_ly, profile_.gamepad_deadzone, lx, ly);
        apply_radial_deadzone(raw_rx, -raw_ry, profile_.gamepad_deadzone, rx, ry);
    }

    void release_everything() noexcept {
        for (std::size_t k = 0; k < static_cast<std::size_t>(kMaxRawInputKeys); ++k) {
            release_raw(static_cast<std::int32_t>(k));
        }
        for (auto& p : gamepads_) {
            const std::uint64_t id = p.id;
            p = GamepadState{};
            p.id = id;
        }
        reset_gyro_filter();
        clear_touch_state();
    }

    void reset_gyro_filter() noexcept {
        gyro_filt_yaw_ = 0.0f;
        gyro_filt_pitch_ = 0.0f;
        gyro_hold_yaw_ = 0.0f;
        gyro_hold_pitch_ = 0.0f;
        gyro_hold_age_ = 0.0f;
    }

    [[nodiscard]] bool has_finger_for(TouchWidgetId w) const noexcept {
        for (const auto& f : fingers_) {
            if (f.widget == w) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool has_classified_finger_for(TouchWidgetId w) const noexcept {
        for (const auto& f : fingers_) {
            if (f.widget == w && f.swipe_classified) {
                return true;
            }
        }
        return false;
    }

    void close_radial() noexcept {
        radial_open_ = false;
        radial_level_ = 0;
        radial_group_ = -1;
        radial_sector_ = -1;
    }

    void clear_touch_state() noexcept {
        fingers_.clear();
        stick_x_ = 0.0f;
        stick_y_ = 0.0f;
        attack_dir_ = TouchCombatDirection::None;
        block_dir_ = TouchCombatDirection::None;
        attack_pulse_dir_ = false;
        block_pulse_dir_ = false;
        close_radial();
        pinch_active_ = false;
        pinch_prev_dist_ = 0.0f;
        pinch_id0_ = -1;
        pinch_id1_ = -1;
    }

    void prune_inactive_touch_state() noexcept {
        for (std::size_t i = fingers_.size(); i > 0U; --i) {
            const TouchWidgetId w = fingers_[i - 1U].widget;
            if (!widget_visible_in_context(w, context_)) {
                fingers_.erase(fingers_.begin() + static_cast<std::ptrdiff_t>(i - 1U));
            }
        }
        if (!widget_visible_in_context(TouchWidgetId::MoveStick, context_) ||
            !has_finger_for(TouchWidgetId::MoveStick)) {
            stick_x_ = 0.0f;
            stick_y_ = 0.0f;
        }
        if (!widget_visible_in_context(TouchWidgetId::AttackZone, context_) ||
            !has_finger_for(TouchWidgetId::AttackZone)) {
            attack_dir_ = TouchCombatDirection::None;
            attack_pulse_dir_ = false;
        }
        if (!widget_visible_in_context(TouchWidgetId::BlockZone, context_) ||
            !has_finger_for(TouchWidgetId::BlockZone)) {
            block_dir_ = TouchCombatDirection::None;
            block_pulse_dir_ = false;
        }
        if (!widget_visible_in_context(TouchWidgetId::CommandRadial, context_)) {
            close_radial();
        }
    }

    void update_pinch_state() noexcept {
        if (context_ != InputContext::CampaignMap || !profile_.pinch_zoom_enabled) {
            pinch_active_ = false;
            pinch_prev_dist_ = 0.0f;
            pinch_id0_ = -1;
            pinch_id1_ = -1;
            return;
        }
        const ActiveFinger* f0 = nullptr;
        const ActiveFinger* f1 = nullptr;
        for (const auto& f : fingers_) {
            if (f.widget == TouchWidgetId::LookArea) {
                if (f0 == nullptr) {
                    f0 = &f;
                } else {
                    f1 = &f;
                    break;
                }
            }
        }
        if (f0 == nullptr || f1 == nullptr) {
            pinch_active_ = false;
            pinch_prev_dist_ = 0.0f;
            pinch_id0_ = -1;
            pinch_id1_ = -1;
            return;
        }
        if (!pinch_active_ || pinch_id0_ != f0->finger_id || pinch_id1_ != f1->finger_id) {
            pinch_active_ = true;
            pinch_id0_ = f0->finger_id;
            pinch_id1_ = f1->finger_id;
            pinch_prev_dist_ =
                std::max(kMinPinchDistance, std::hypot((f1->last_x - f0->last_x) * viewport_aspect_,
                                                       f1->last_y - f0->last_y));
        }
    }

    static void apply_radial_deadzone(float x,
                                      float y,
                                      float deadzone,
                                      float& out_x,
                                      float& out_y) noexcept {
        const float mag = std::sqrt(x * x + y * y);
        if (mag <= deadzone || deadzone >= 1.0f) {
            out_x = 0.0f;
            out_y = 0.0f;
            return;
        }
        const float capped = std::min(mag, 1.0f);
        const float norm = (capped - deadzone) / (1.0f - deadzone);
        const float scale = norm / mag;
        out_x = x * scale;
        out_y = y * scale;
    }

    // `dx` / `dy` in screen-height units.
    [[nodiscard]] static TouchCombatDirection classify_swipe(float dx,
                                                             float dy,
                                                             float threshold) noexcept {
        if (std::max(std::fabs(dx), std::fabs(dy)) < threshold) {
            return TouchCombatDirection::None;
        }
        if (std::fabs(dx) >= std::fabs(dy)) {
            return (dx > 0.0f) ? TouchCombatDirection::Right : TouchCombatDirection::Left;
        }
        // Normalized screen coordinates have Y=0 at top, Y=1 at bottom.
        return (dy < 0.0f) ? TouchCombatDirection::Up : TouchCombatDirection::Down;
    }

    // `dx` / `dy` in screen-height units.
    // Returns `0..3` clockwise from North: 0=North(All), 1=East(Infantry),
    // 2=South(Archers), 3=West(Cavalry).
    [[nodiscard]] static std::int32_t classify_radial_quadrant(float dx,
                                                               float dy,
                                                               float deadzone) noexcept {
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < deadzone) {
            return -1;
        }
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 2.0f * kPi;
        float angle = std::atan2(dx, -dy);
        if (angle < 0.0f) {
            angle += kTwoPi;
        }
        const float shifted = angle + (kPi / 4.0f);
        const int idx = static_cast<int>(std::floor(shifted / (kPi / 2.0f))) % 4;
        return static_cast<std::int32_t>(idx);
    }

    // `dx` / `dy` in screen-height units.
    [[nodiscard]] static std::int32_t classify_radial_sector(float dx,
                                                             float dy,
                                                             float deadzone) noexcept {
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist < deadzone) {
            return -1;
        }
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 2.0f * kPi;
        // 0 rad = North (dy < 0), increasing clockwise (+dx).
        float angle = std::atan2(dx, -dy);
        if (angle < 0.0f) {
            angle += kTwoPi;
        }
        const float shifted = angle + (kPi / 8.0f);
        const int idx = static_cast<int>(std::floor(shifted / (kPi / 4.0f))) % 8;
        return static_cast<std::int32_t>(idx);
    }

    [[nodiscard]] float radial_ring1_outer_radius(const TouchWidgetLayout& rad,
                                                  float aspect) const noexcept {
        return std::max(profile_.radial_deadzone * 2.5f, rad.circle_radius(aspect) * 0.85f);
    }

    [[nodiscard]] float radial_outer_hit_radius(const TouchWidgetLayout& rad,
                                                float aspect) const noexcept {
        return std::max(rad.circle_radius(aspect) * 2.2f,
                        radial_ring1_outer_radius(rad, aspect) + 0.08f);
    }

    static constexpr std::array<GameKeyId, 4> kRadialGroupKeys = {
        GameKeyId::ControlEveryone,
        GameKeyId::ControlGroup1,
        GameKeyId::ControlGroup2,
        GameKeyId::ControlGroup3,
    };

    static constexpr std::array<GameKeyId, 8> kRadialOrderKeys = {
        GameKeyId::Order1, GameKeyId::Order2, GameKeyId::Order3, GameKeyId::Order4,
        GameKeyId::Order5, GameKeyId::Order6, GameKeyId::Order7, GameKeyId::Order8,
    };

    [[nodiscard]] TouchWidgetId pick_widget_for_touch(float x, float y) const noexcept {
        if (radial_open_ && radial_level_ == 2 && !has_finger_for(TouchWidgetId::CommandRadial)) {
            const TouchWidgetLayout& rad =
                profile_.widgets[static_cast<std::size_t>(TouchWidgetId::CommandRadial)];
            if (is_touch_widget_active(TouchWidgetId::CommandRadial, context_, rad, true)) {
                const float r_hit = radial_outer_hit_radius(rad, viewport_aspect_);
                const float px = (x - rad.cx) * viewport_aspect_;
                const float py = y - rad.cy;
                if ((px * px + py * py) <= r_hit * r_hit) {
                    return TouchWidgetId::CommandRadial;
                }
            }
        }
        for (const TouchWidgetId id : kTouchWidgetHitOrder) {
            const TouchWidgetLayout& w = profile_.widgets[static_cast<std::size_t>(id)];
            if (is_touch_widget_active(id, context_, w, true) &&
                w.contains(x, y, viewport_aspect_)) {
                return id;
            }
        }
        return TouchWidgetId::Count;
    }

    void update_stick_from_point(float x, float y) noexcept {
        const auto& w = profile_.widgets[static_cast<std::size_t>(TouchWidgetId::MoveStick)];
        const float a = viewport_aspect_;
        const float r = w.circle_radius(a);
        const float raw_x = (x - w.cx) * a / r;
        const float raw_y = -(y - w.cy) / r;  // Screen top = forward (+Y)
        apply_radial_deadzone(raw_x, raw_y, profile_.stick_deadzone, stick_x_, stick_y_);
    }

    void resolve_delayed_swipes(float aspect) noexcept {
        for (auto& f : fingers_) {
            if ((f.widget != TouchWidgetId::AttackZone && f.widget != TouchWidgetId::BlockZone) ||
                f.swipe_classified || f.elapsed_seconds < (kSwipeClassifyDelaySeconds - 1e-5f)) {
                continue;
            }
            const float dx = (f.last_x - f.anchor_x) * aspect;
            const float dy = f.last_y - f.anchor_y;
            TouchCombatDirection dir = TouchCombatDirection::None;
            if (std::max(std::fabs(dx), std::fabs(dy)) > 1e-4f) {
                dir = classify_swipe(dx, dy, 0.0f);
            }
            if (dir == TouchCombatDirection::None) {
                dir = (f.widget == TouchWidgetId::AttackZone) ? profile_.default_attack_dir
                                                            : profile_.default_block_dir;
            }
            f.swipe_classified = true;
            f.swipe_dir = dir;
            if (f.widget == TouchWidgetId::AttackZone) {
                attack_dir_ = dir;
            } else {
                block_dir_ = dir;
            }
        }
    }

    void handle_touch_down(std::int64_t finger_id, float x, float y) {
        // A repeated DOWN for a tracked finger (missed UP) restarts that finger.
        handle_touch_up(finger_id, false);
        if (fingers_.size() >= kMaxTrackedFingers) {
            return;
        }
        TouchWidgetId target = pick_widget_for_touch(x, y);
        if (radial_open_ && radial_level_ == 2 && !has_finger_for(TouchWidgetId::CommandRadial) &&
            (target == TouchWidgetId::LookArea || target == TouchWidgetId::Count)) {
            close_radial();
        }
        bool look_enabled = (target == TouchWidgetId::LookArea);
        if (target == TouchWidgetId::Count) {
            if (context_ == InputContext::CampaignMap && profile_.pinch_zoom_enabled) {
                target = TouchWidgetId::LookArea;
                look_enabled = false;
            } else {
                return;
            }
        }
        ActiveFinger af{};
        af.finger_id = finger_id;
        af.widget = target;
        af.anchor_x = x;
        af.anchor_y = y;
        af.last_x = x;
        af.last_y = y;
        af.radial_origin_x = x;
        af.radial_origin_y = y;
        af.down_frame = frame_counter_;
        af.look_enabled = look_enabled;
        if (target == TouchWidgetId::CommandRadial) {
            if (!radial_open_ || radial_level_ == 0) {
                radial_open_ = true;
                radial_level_ = 1;
                radial_group_ = -1;
                radial_sector_ = -1;
                virtual_pulse_gk_[static_cast<std::size_t>(GameKeyId::ViewOrders)] = true;
            } else if (radial_level_ == 2) {
                const auto& rad =
                    profile_.widgets[static_cast<std::size_t>(TouchWidgetId::CommandRadial)];
                af.radial_origin_x = rad.cx;
                af.radial_origin_y = rad.cy;
                radial_sector_ = classify_radial_sector(
                    (x - rad.cx) * viewport_aspect_, y - rad.cy, profile_.radial_deadzone);
            }
        }
        fingers_.push_back(af);
        if (target == TouchWidgetId::MoveStick) {
            update_stick_from_point(x, y);
        } else if (target == TouchWidgetId::LookArea) {
            update_pinch_state();
        }
    }

    void handle_touch_move(std::int64_t finger_id,
                           float x,
                           float y,
                           float& look_dx,
                           float& look_dy,
                           float& map_zoom_delta) noexcept {
        const float a = viewport_aspect_;
        for (auto& f : fingers_) {
            if (f.finger_id != finger_id) {
                continue;
            }
            if (f.widget == TouchWidgetId::MoveStick) {
                update_stick_from_point(x, y);
            } else if (f.widget == TouchWidgetId::LookArea) {
                if (context_ == InputContext::CampaignMap && profile_.pinch_zoom_enabled &&
                    pinch_active_) {
                    f.last_x = x;
                    f.last_y = y;
                    if (finger_id == pinch_id0_ || finger_id == pinch_id1_) {
                        const ActiveFinger* f0 = nullptr;
                        const ActiveFinger* f1 = nullptr;
                        for (const auto& cand : fingers_) {
                            if (cand.finger_id == pinch_id0_) {
                                f0 = &cand;
                            } else if (cand.finger_id == pinch_id1_) {
                                f1 = &cand;
                            }
                        }
                        if (f0 != nullptr && f1 != nullptr) {
                            const float new_d =
                                std::max(kMinPinchDistance,
                                         std::hypot((f1->last_x - f0->last_x) * a,
                                                    f1->last_y - f0->last_y));
                            map_zoom_delta += std::log(new_d / pinch_prev_dist_) *
                                              profile_.pinch_zoom_sensitivity * kPinchZoomScale;
                            pinch_prev_dist_ = new_d;
                        }
                    }
                    return;
                }
                if (f.look_enabled) {
                    look_dx += (x - f.last_x) * a;
                    look_dy += (y - f.last_y);
                }
            } else if (f.widget == TouchWidgetId::AttackZone ||
                       f.widget == TouchWidgetId::BlockZone) {
                const auto dir = classify_swipe((x - f.anchor_x) * a, y - f.anchor_y,
                                                profile_.swipe_threshold);
                if (dir != TouchCombatDirection::None) {
                    f.swipe_classified = true;
                    f.swipe_dir = dir;
                    if (f.widget == TouchWidgetId::AttackZone) {
                        attack_dir_ = dir;
                    } else {
                        block_dir_ = dir;
                    }
                }
            } else if (f.widget == TouchWidgetId::CommandRadial) {
                const float dx = (x - f.radial_origin_x) * a;
                const float dy = y - f.radial_origin_y;
                if (radial_level_ == 1) {
                    const std::int32_t q =
                        classify_radial_quadrant(dx, dy, profile_.radial_deadzone);
                    const auto& rad =
                        profile_.widgets[static_cast<std::size_t>(TouchWidgetId::CommandRadial)];
                    if (std::hypot(dx, dy) < radial_ring1_outer_radius(rad, a)) {
                        radial_group_ = q;
                    } else {
                        // Leaving ring 1 commits the group last hovered inside it (the finger
                        // may cross into another quadrant on the way out); else the exit one.
                        if (radial_group_ < 0 || radial_group_ >= 4) {
                            radial_group_ = (q >= 0 && q < 4) ? q : 0;
                        }
                        const auto gk = kRadialGroupKeys[static_cast<std::size_t>(radial_group_)];
                        virtual_pulse_gk_[static_cast<std::size_t>(gk)] = true;
                        radial_level_ = 2;
                        radial_sector_ =
                            classify_radial_sector(dx, dy, profile_.radial_deadzone);
                    }
                } else if (radial_level_ == 2) {
                    radial_sector_ = classify_radial_sector(dx, dy, profile_.radial_deadzone);
                }
            }
            f.last_x = x;
            f.last_y = y;
            return;
        }
    }

    // `commit == false` (cancelled touch) releases the finger without issuing the
    // highlighted radial order or button tap.
    void handle_touch_up(std::int64_t finger_id, bool commit) noexcept {
        for (std::size_t i = 0; i < fingers_.size(); ++i) {
            if (fingers_[i].finger_id != finger_id) {
                continue;
            }
            const ActiveFinger released = fingers_[i];
            const TouchWidgetId w = released.widget;
            const bool same_frame = (released.down_frame == frame_counter_);
            if (w == TouchWidgetId::AttackZone || w == TouchWidgetId::BlockZone) {
                const bool was_classified = released.swipe_classified;
                if (commit && (!was_classified || same_frame)) {
                    const GameKeyId gk = (w == TouchWidgetId::AttackZone) ? GameKeyId::Attack
                                                                         : GameKeyId::Defend;
                    virtual_pulse_gk_[static_cast<std::size_t>(gk)] = true;
                    if (!was_classified) {
                        if (w == TouchWidgetId::AttackZone) {
                            attack_dir_ = profile_.default_attack_dir;
                            attack_pulse_dir_ = true;
                        } else {
                            block_dir_ = profile_.default_block_dir;
                            block_pulse_dir_ = true;
                        }
                    }
                }
            } else if (w != TouchWidgetId::MoveStick && w != TouchWidgetId::LookArea &&
                       w != TouchWidgetId::CommandRadial) {
                if (commit) {
                    if (same_frame) {
                        if (const auto gk = touch_widget_game_key(w); gk.has_value()) {
                            virtual_pulse_gk_[static_cast<std::size_t>(*gk)] = true;
                        }
                    }
                    if (w == TouchWidgetId::MapFollow) {
                        map_follow_ = !map_follow_;
                    } else if (w == TouchWidgetId::MapPanMode) {
                        map_pan_mode_ = !map_pan_mode_;
                    } else if (w == TouchWidgetId::MapWait) {
                        map_paused_ = !map_paused_;
                    }
                }
            }
            fingers_.erase(fingers_.begin() + static_cast<std::ptrdiff_t>(i));
            if (w == TouchWidgetId::MoveStick && !has_finger_for(TouchWidgetId::MoveStick)) {
                stick_x_ = 0.0f;
                stick_y_ = 0.0f;
            } else if (w == TouchWidgetId::LookArea) {
                update_pinch_state();
            } else if (w == TouchWidgetId::AttackZone &&
                       !has_finger_for(TouchWidgetId::AttackZone)) {
                if (!attack_pulse_dir_) {
                    attack_dir_ = TouchCombatDirection::None;
                }
            } else if (w == TouchWidgetId::BlockZone &&
                       !has_finger_for(TouchWidgetId::BlockZone)) {
                if (!block_pulse_dir_) {
                    block_dir_ = TouchCombatDirection::None;
                }
            } else if (w == TouchWidgetId::CommandRadial &&
                       !has_finger_for(TouchWidgetId::CommandRadial)) {
                if (!commit) {
                    close_radial();
                } else if (radial_level_ == 1) {
                    if (radial_group_ >= 0 && radial_group_ < 4) {
                        const auto gk = kRadialGroupKeys[static_cast<std::size_t>(radial_group_)];
                        virtual_pulse_gk_[static_cast<std::size_t>(gk)] = true;
                        radial_open_ = true;
                        radial_level_ = 2;
                        radial_sector_ = -1;
                    } else {
                        close_radial();
                    }
                } else if (radial_level_ == 2) {
                    if (radial_sector_ >= 0 && radial_sector_ < 8) {
                        const auto gk =
                            kRadialOrderKeys[static_cast<std::size_t>(radial_sector_)];
                        virtual_pulse_gk_[static_cast<std::size_t>(gk)] = true;
                    }
                    close_radial();
                } else {
                    close_radial();
                }
            }
            return;
        }
    }

    void synthesize_overlay_game_keys(std::array<bool, kNumGameInputKeys>& out) const noexcept {
        const float thresh = profile_.stick_key_threshold;
        const bool on_map = (context_ == InputContext::CampaignMap);
        const auto press = [&](GameKeyId mission_key, GameKeyId map_key) {
            out[static_cast<std::size_t>(mission_key)] = true;
            if (on_map) {
                out[static_cast<std::size_t>(map_key)] = true;
            }
        };
        const auto emit_stick = [&](float x, float y) {
            if (y >= thresh) {
                press(GameKeyId::MoveForward, GameKeyId::MapMoveForward);
            }
            if (y <= -thresh) {
                press(GameKeyId::MoveBackward, GameKeyId::MapMoveBackward);
            }
            if (x <= -thresh) {
                press(GameKeyId::MoveLeft, GameKeyId::MapMoveLeft);
            }
            if (x >= thresh) {
                press(GameKeyId::MoveRight, GameKeyId::MapMoveRight);
            }
        };
        // Virtual stick and physical gamepad left stick -> Mission and CampaignMap move keys.
        emit_stick(stick_x_, stick_y_);
        float glx = 0.0f;
        float gly = 0.0f;
        float grx = 0.0f;
        float gry = 0.0f;
        gamepad_sticks(glx, gly, grx, gry);
        emit_stick(glx, gly);

        for (std::size_t i = 0; i < kNumTouchWidgets; ++i) {
            const auto wid = static_cast<TouchWidgetId>(i);
            const auto gk = touch_widget_game_key(wid);
            if (!gk.has_value()) {
                continue;
            }
            if (wid == TouchWidgetId::AttackZone || wid == TouchWidgetId::BlockZone) {
                if (has_classified_finger_for(wid)) {
                    out[static_cast<std::size_t>(*gk)] = true;
                }
            } else if (has_finger_for(wid)) {
                out[static_cast<std::size_t>(*gk)] = true;
            }
        }
    }

    WseGameKeyManager key_mgr_{};
    TouchOverlayProfile profile_{};
    TouchLayoutEditor editor_{};
    InputContext context_ = InputContext::Mission;
    float viewport_aspect_ = 1.0f;
    bool mouse_look_enabled_ = true;

    std::array<bool, kMaxRawInputKeys> raw_down_{};
    std::array<bool, kMaxRawInputKeys> raw_prev_down_{};
    std::array<bool, kMaxRawInputKeys> raw_clicked_{};
    std::array<bool, kMaxRawInputKeys> raw_released_{};
    std::array<bool, kMaxRawInputKeys> omitted_keys_{};
    std::array<bool, kMaxRawInputKeys> suppressed_keys_{};

    std::array<bool, kNumGameInputKeys> game_down_{};
    std::array<bool, kNumGameInputKeys> game_prev_down_{};
    std::array<bool, kNumGameInputKeys> game_clicked_{};
    std::array<bool, kNumGameInputKeys> game_released_{};
    std::array<bool, kNumGameInputKeys> virtual_held_{};
    std::array<bool, kNumGameInputKeys> virtual_prev_held_{};
    std::array<bool, kNumGameInputKeys> virtual_pulse_gk_{};
    std::array<bool, kNumGameInputKeys> pulse_prev_{};

    std::vector<GamepadState> gamepads_{};
    bool gamepad_active_ = false;
    std::vector<ActiveFinger> fingers_{};
    std::vector<RawInputEvent> poll_scratch_{};

    float stick_x_ = 0.0f;
    float stick_y_ = 0.0f;
    TouchCombatDirection attack_dir_ = TouchCombatDirection::None;
    TouchCombatDirection block_dir_ = TouchCombatDirection::None;
    bool attack_pulse_dir_ = false;
    bool block_pulse_dir_ = false;
    std::int32_t radial_level_ = 0;
    std::int32_t radial_group_ = -1;
    std::int32_t radial_sector_ = -1;
    bool radial_open_ = false;
    bool pinch_active_ = false;
    float pinch_prev_dist_ = 0.0f;
    std::int64_t pinch_id0_ = -1;
    std::int64_t pinch_id1_ = -1;
    bool map_follow_ = true;
    bool map_pan_mode_ = false;
    bool map_paused_ = false;
    float gyro_filt_yaw_ = 0.0f;
    float gyro_filt_pitch_ = 0.0f;
    float gyro_hold_yaw_ = 0.0f;  // last frame-average rate, held between sensor samples
    float gyro_hold_pitch_ = 0.0f;
    float gyro_hold_age_ = 0.0f;  // seconds since the last gyro sample
    float cursor_x_ = 0.5f;
    float cursor_y_ = 0.5f;
    std::uint64_t frame_counter_ = 0U;
    InputFrameState frame_{};
};

// Free-camera controller for the P4 BRF mesh viewer (`docs/PLATFORM_PLAN.md`, slice P7).
// Uses Y-up world orientation matching `make_orbit_mvp_matrix` in `wse/platform/rhi.h`:
// `yaw = 0, pitch = 0` at `pos = (0, 0, +1.8)` looks down `-Z` toward the origin.
struct FreeCameraController {
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 1.8f;
    float yaw_rad = 0.0f;
    float pitch_rad = 0.0f;
    float move_speed = 2.0f;
    float near_plane = 0.1f;
    float far_plane = 400.0f;

    void apply_input(const InputFrameState& input, float dt_seconds) noexcept {
        if (std::isfinite(input.look_yaw_delta)) {
            // Wrapped to (-pi, pi] so float precision does not decay over long sessions.
            yaw_rad = std::remainder(yaw_rad + input.look_yaw_delta, 6.283185307179586f);
        }
        if (std::isfinite(input.look_pitch_delta)) {
            pitch_rad = std::clamp(pitch_rad + input.look_pitch_delta, -1.48f, 1.48f);
        }
        if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0f) {
            return;
        }
        const float cp = std::cos(pitch_rad);
        const float sp = std::sin(pitch_rad);
        const float sy = std::sin(yaw_rad);
        const float cy = std::cos(yaw_rad);
        // Forward matches `(0, 0, -1)` at yaw=0, pitch=0; right is `(1, 0, 0)`.
        const float fx = sy * cp;
        const float fy = sp;
        const float fz = -cy * cp;
        const float rx = cy;
        const float rz = sy;
        const float mx = std::isfinite(input.move_x) ? std::clamp(input.move_x, -1.0f, 1.0f) : 0.0f;
        const float my = std::isfinite(input.move_y) ? std::clamp(input.move_y, -1.0f, 1.0f) : 0.0f;
        const float step = move_speed * dt_seconds;
        pos_x += (fx * my + rx * mx) * step;
        pos_y += (fy * my) * step;
        pos_z += (fz * my + rz * mx) * step;
    }

    // Builds the Vulkan clip-space (`-Y` up screen, `[0, 1]` Z) column-major MVP matrix.
    void build_mvp_matrix(float aspect, float fov_deg, float out_mvp[16]) const noexcept {
        const float safe_aspect = (std::isfinite(aspect) && aspect > 0.05f) ? aspect : 1.0f;
        const float safe_fov =
            std::clamp(std::isfinite(fov_deg) ? fov_deg : 60.0f, 15.0f, 150.0f);
        const float f = 1.0f / std::tan(safe_fov * 0.5f * 0.017453292519943295f);

        const float cp = std::cos(pitch_rad);
        const float sp = std::sin(pitch_rad);
        const float sy = std::sin(yaw_rad);
        const float cy = std::cos(yaw_rad);

        const float fx = sy * cp;
        const float fy = sp;
        const float fz = -cy * cp;

        // Right = normalize(cross(forward, world_up(0, 1, 0))) = (cy, 0, sy).
        const float rx = cy;
        const float ry = 0.0f;
        const float rz = sy;

        // Up = cross(right, forward)
        const float ux = ry * fz - rz * fy;
        const float uy = rz * fx - rx * fz;
        const float uz = rx * fy - ry * fx;

        const float tx = -(rx * pos_x + ry * pos_y + rz * pos_z);
        const float ty = -(ux * pos_x + uy * pos_y + uz * pos_z);
        const float tz = (fx * pos_x + fy * pos_y + fz * pos_z);

        const float view[16] = {
            rx, ux, -fx, 0.0f,
            ry, uy, -fy, 0.0f,
            rz, uz, -fz, 0.0f,
            tx, ty,  tz, 1.0f,
        };

        const float zn = std::max(0.01f, near_plane);
        const float zf = std::max(zn + 1.0f, far_plane);
        const float a = zf / (zn - zf);
        const float b = (zf * zn) / (zn - zf);
        const float proj[16] = {
            f / safe_aspect, 0.0f, 0.0f, 0.0f,
            0.0f, -f, 0.0f, 0.0f,
            0.0f, 0.0f, a, -1.0f,
            0.0f, 0.0f, b, 0.0f,
        };

        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += proj[k * 4 + row] * view[col * 4 + k];
                }
                out_mvp[col * 4 + row] = sum;
            }
        }
    }

    // Exports into the Warband camera frame (`s`=right, `f`=up, `-u`=forward, `o`=pos),
    // the convention `mbSoundManager::setTransform` (`0x140333730`) consumes (see
    // `listener_from_camera_transform` in `wse/platform/audio.h`).
    [[nodiscard]] wse::core::WseTransform to_transform() const noexcept {
        const float cp = std::cos(pitch_rad);
        const float sp = std::sin(pitch_rad);
        const float sy = std::sin(yaw_rad);
        const float cy = std::cos(yaw_rad);
        const float fx = sy * cp;
        const float fy = sp;
        const float fz = -cy * cp;
        const float rx = cy;
        const float rz = sy;
        const float ux = -rz * fy;
        const float uy = rz * fx - rx * fz;
        const float uz = rx * fy;
        wse::core::WseTransform t{};
        t.s = {rx, 0.0f, rz, 0.0f};
        t.f = {ux, uy, uz, 0.0f};
        t.u = {-fx, -fy, -fz, 0.0f};
        t.o = {pos_x, pos_y, pos_z, 1.0f};
        return t;
    }
};

}  // namespace wse::platform
