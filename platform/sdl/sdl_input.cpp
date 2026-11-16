// SDL3 keyboard, mouse, gamepad, and multi-touch backend (Slice P7).

#include "sdl_input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace wse::platform {

namespace {

// Degrees clockwise from portrait, or -1 when SDL does not know.
[[nodiscard]] int sdl_orientation_degrees(SDL_DisplayOrientation o) noexcept {
    switch (o) {
    case SDL_ORIENTATION_PORTRAIT:
        return 0;
    case SDL_ORIENTATION_LANDSCAPE:
        return 90;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED:
        return 180;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
        return 270;
    case SDL_ORIENTATION_UNKNOWN:
    default:
        return -1;
    }
}

[[nodiscard]] std::int32_t sdl_scancode_to_raw_key(SDL_Scancode sc) noexcept {
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return RawKey::kEscape;
    case SDL_SCANCODE_1: return RawKey::k1;
    case SDL_SCANCODE_2: return RawKey::k2;
    case SDL_SCANCODE_3: return RawKey::k3;
    case SDL_SCANCODE_4: return RawKey::k4;
    case SDL_SCANCODE_5: return RawKey::k5;
    case SDL_SCANCODE_6: return RawKey::k6;
    case SDL_SCANCODE_7: return RawKey::k7;
    case SDL_SCANCODE_8: return RawKey::k8;
    case SDL_SCANCODE_9: return RawKey::k9;
    case SDL_SCANCODE_0: return RawKey::k0;
    case SDL_SCANCODE_MINUS: return RawKey::kMinus;
    case SDL_SCANCODE_EQUALS: return RawKey::kEquals;
    case SDL_SCANCODE_BACKSPACE: return RawKey::kBackSpace;
    case SDL_SCANCODE_TAB: return RawKey::kTab;
    case SDL_SCANCODE_Q: return RawKey::kQ;
    case SDL_SCANCODE_W: return RawKey::kW;
    case SDL_SCANCODE_E: return RawKey::kE;
    case SDL_SCANCODE_R: return RawKey::kR;
    case SDL_SCANCODE_T: return RawKey::kT;
    case SDL_SCANCODE_Y: return RawKey::kY;
    case SDL_SCANCODE_U: return RawKey::kU;
    case SDL_SCANCODE_I: return RawKey::kI;
    case SDL_SCANCODE_O: return RawKey::kO;
    case SDL_SCANCODE_P: return RawKey::kP;
    case SDL_SCANCODE_LEFTBRACKET: return RawKey::kLBracket;
    case SDL_SCANCODE_RIGHTBRACKET: return RawKey::kRBracket;
    case SDL_SCANCODE_RETURN: return RawKey::kEnter;
    case SDL_SCANCODE_LCTRL: return RawKey::kLeftCtrl;
    case SDL_SCANCODE_A: return RawKey::kA;
    case SDL_SCANCODE_S: return RawKey::kS;
    case SDL_SCANCODE_D: return RawKey::kD;
    case SDL_SCANCODE_F: return RawKey::kF;
    case SDL_SCANCODE_G: return RawKey::kG;
    case SDL_SCANCODE_H: return RawKey::kH;
    case SDL_SCANCODE_J: return RawKey::kJ;
    case SDL_SCANCODE_K: return RawKey::kK;
    case SDL_SCANCODE_L: return RawKey::kL;
    case SDL_SCANCODE_SEMICOLON: return RawKey::kSemicolon;
    case SDL_SCANCODE_APOSTROPHE: return RawKey::kApostrophe;
    case SDL_SCANCODE_GRAVE: return RawKey::kGrave;
    case SDL_SCANCODE_LSHIFT: return RawKey::kLeftShift;
    case SDL_SCANCODE_BACKSLASH: return RawKey::kBackslash;
    case SDL_SCANCODE_Z: return RawKey::kZ;
    case SDL_SCANCODE_X: return RawKey::kX;
    case SDL_SCANCODE_C: return RawKey::kC;
    case SDL_SCANCODE_V: return RawKey::kV;
    case SDL_SCANCODE_B: return RawKey::kB;
    case SDL_SCANCODE_N: return RawKey::kN;
    case SDL_SCANCODE_M: return RawKey::kM;
    case SDL_SCANCODE_COMMA: return RawKey::kComma;
    case SDL_SCANCODE_PERIOD: return RawKey::kPeriod;
    case SDL_SCANCODE_SLASH: return RawKey::kSlash;
    case SDL_SCANCODE_RSHIFT: return RawKey::kRightShift;
    case SDL_SCANCODE_KP_MULTIPLY: return RawKey::kNumpadStar;
    case SDL_SCANCODE_LALT: return RawKey::kLeftAlt;
    case SDL_SCANCODE_SPACE: return RawKey::kSpace;
    case SDL_SCANCODE_CAPSLOCK: return RawKey::kCapsLock;
    case SDL_SCANCODE_F1: return RawKey::kF1;
    case SDL_SCANCODE_F2: return RawKey::kF2;
    case SDL_SCANCODE_F3: return RawKey::kF3;
    case SDL_SCANCODE_F4: return RawKey::kF4;
    case SDL_SCANCODE_F5: return RawKey::kF5;
    case SDL_SCANCODE_F6: return RawKey::kF6;
    case SDL_SCANCODE_F7: return RawKey::kF7;
    case SDL_SCANCODE_F8: return RawKey::kF8;
    case SDL_SCANCODE_F9: return RawKey::kF9;
    case SDL_SCANCODE_F10: return RawKey::kF10;
    case SDL_SCANCODE_NUMLOCKCLEAR: return RawKey::kNumLock;
    case SDL_SCANCODE_KP_7: return RawKey::kNumpad7;
    case SDL_SCANCODE_KP_8: return RawKey::kNumpad8;
    case SDL_SCANCODE_KP_9: return RawKey::kNumpad9;
    case SDL_SCANCODE_KP_MINUS: return RawKey::kNumpadMinus;
    case SDL_SCANCODE_KP_4: return RawKey::kNumpad4;
    case SDL_SCANCODE_KP_5: return RawKey::kNumpad5;
    case SDL_SCANCODE_KP_6: return RawKey::kNumpad6;
    case SDL_SCANCODE_KP_PLUS: return RawKey::kNumpadPlus;
    case SDL_SCANCODE_KP_1: return RawKey::kNumpad1;
    case SDL_SCANCODE_KP_2: return RawKey::kNumpad2;
    case SDL_SCANCODE_KP_3: return RawKey::kNumpad3;
    case SDL_SCANCODE_KP_0: return RawKey::kNumpad0;
    case SDL_SCANCODE_KP_PERIOD: return RawKey::kNumpadDot;
    case SDL_SCANCODE_F11: return RawKey::kF11;
    case SDL_SCANCODE_F12: return RawKey::kF12;
    case SDL_SCANCODE_KP_ENTER: return RawKey::kNumpadEnter;
    case SDL_SCANCODE_RCTRL: return RawKey::kRightCtrl;
    case SDL_SCANCODE_KP_DIVIDE: return RawKey::kNumpadSlash;
    case SDL_SCANCODE_PRINTSCREEN: return RawKey::kPrintScreen;
    case SDL_SCANCODE_RALT: return RawKey::kRightAlt;
    case SDL_SCANCODE_HOME: return RawKey::kHome;
    case SDL_SCANCODE_UP: return RawKey::kUpArrow;
    case SDL_SCANCODE_PAGEUP: return RawKey::kPageUp;
    case SDL_SCANCODE_LEFT: return RawKey::kLeftArrow;
    case SDL_SCANCODE_RIGHT: return RawKey::kRightArrow;
    case SDL_SCANCODE_END: return RawKey::kEnd;
    case SDL_SCANCODE_DOWN: return RawKey::kDownArrow;
    case SDL_SCANCODE_PAGEDOWN: return RawKey::kPageDown;
    case SDL_SCANCODE_INSERT: return RawKey::kInsert;
    case SDL_SCANCODE_DELETE: return RawKey::kDelete;
    // Android's system Back button opens / closes menus like Esc on PC.
    case SDL_SCANCODE_AC_BACK: return RawKey::kEscape;
    default: return -1;
    }
}

[[nodiscard]] std::int32_t sdl_mouse_button_to_raw_key(std::uint8_t btn) noexcept {
    switch (btn) {
    case SDL_BUTTON_LEFT: return RawKey::kLeftMouse;
    case SDL_BUTTON_RIGHT: return RawKey::kRightMouse;
    case SDL_BUTTON_MIDDLE: return RawKey::kMiddleMouse;
    case SDL_BUTTON_X1: return RawKey::kMouseBtn4;
    case SDL_BUTTON_X2: return RawKey::kMouseBtn5;
    default: return -1;
    }
}

[[nodiscard]] std::int32_t sdl_gamepad_button_to_raw_key(std::uint8_t btn) noexcept {
    switch (btn) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return RawKey::kGamepadA;
    case SDL_GAMEPAD_BUTTON_EAST: return RawKey::kGamepadB;
    case SDL_GAMEPAD_BUTTON_WEST: return RawKey::kGamepadX;
    case SDL_GAMEPAD_BUTTON_NORTH: return RawKey::kGamepadY;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return RawKey::kDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return RawKey::kDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return RawKey::kDpadRight;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return RawKey::kDpadLeft;
    case SDL_GAMEPAD_BUTTON_START: return RawKey::kGamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return RawKey::kGamepadBack;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return RawKey::kRightBumper;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return RawKey::kLeftBumper;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return RawKey::kRightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return RawKey::kLeftStick;
    default: return -1;
    }
}

[[nodiscard]] float normalize_sdl_signed_axis(std::int16_t raw) noexcept {
    const float v = static_cast<float>(raw) / (raw < 0 ? 32768.0f : 32767.0f);
    return std::clamp(v, -1.0f, 1.0f);
}

[[nodiscard]] float normalize_sdl_trigger(std::int16_t raw) noexcept {
    return std::clamp(static_cast<float>(raw) / 32767.0f, 0.0f, 1.0f);
}

class SdlInputBackendImpl final : public SdlInputBackend {
public:
    SdlInputBackendImpl() {
        if ((SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) == 0U) {
            owns_gamepad_subsys_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        }
        int pad_count = 0;
        if (SDL_JoystickID* pads = SDL_GetGamepads(&pad_count); pads != nullptr) {
            for (int i = 0; i < pad_count; ++i) {
                const SDL_JoystickID jid = pads[i];
                if (SDL_Gamepad* pad = SDL_OpenGamepad(jid); pad != nullptr) {
                    open_pads_.emplace(jid, pad);
                    RawInputEvent out{};
                    out.type = RawInputEvent::Type::GamepadConnected;
                    out.device_id = static_cast<std::uint64_t>(jid);
                    queue_.push_back(out);
                }
            }
            SDL_free(pads);
        }
    }

    ~SdlInputBackendImpl() override {
        std::lock_guard<std::mutex> lock(mutex_);
        close_gyro_locked();
        for (auto& [id, pad] : open_pads_) {
            (void)id;
            if (pad != nullptr) {
                SDL_CloseGamepad(pad);
            }
        }
        open_pads_.clear();
        if (owns_gamepad_subsys_) {
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }
    }

    void set_gyro_enabled(bool enabled) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gyro_enabled_ == enabled) {
            return;
        }
        gyro_enabled_ = enabled;
        if (gyro_enabled_) {
            refresh_rotation_locked();
            open_gyro_locked();
        } else {
            close_gyro_locked();
        }
        for (auto& [jid, pad] : open_pads_) {
            set_pad_gyro_locked(jid, pad, gyro_enabled_);
        }
    }

    [[nodiscard]] bool is_gyro_enabled() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return gyro_enabled_;
    }

    [[nodiscard]] bool has_gyro_sensor() const noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return open_gyro_ != nullptr || !gyro_pads_.empty();
    }

    // Fallback size; events carrying a window id are normalized by that window's
    // current size, so the first frame and resizes never use a stale viewport.
    void set_viewport_size(int width, int height) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        viewport_w_ = std::max(width, 1);
        viewport_h_ = std::max(height, 1);
    }

    bool push_sdl_event(const void* sdl_event) override {
        if (sdl_event == nullptr) {
            return false;
        }
        const auto& ev = *static_cast<const SDL_Event*>(sdl_event);
        std::lock_guard<std::mutex> lock(mutex_);
        float vw = static_cast<float>(viewport_w_);
        float vh = static_cast<float>(viewport_h_);
        if (SDL_Window* win = SDL_GetWindowFromEvent(&ev); win != nullptr) {
            int w = 0;
            int h = 0;
            if (SDL_GetWindowSize(win, &w, &h) && w > 0 && h > 0) {
                vw = static_cast<float>(w);
                vh = static_cast<float>(h);
            }
        }

        switch (ev.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const std::int32_t rk = sdl_scancode_to_raw_key(ev.key.scancode);
            if (rk < 0) {
                return false;
            }
            RawInputEvent out{};
            out.type = (ev.type == SDL_EVENT_KEY_DOWN) ? RawInputEvent::Type::KeyDown
                                                       : RawInputEvent::Type::KeyUp;
            out.raw_key = rk;
            out.is_repeat = ev.key.repeat;
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            if (ev.button.which == SDL_TOUCH_MOUSEID) {
                return false;
            }
            const std::int32_t rk = sdl_mouse_button_to_raw_key(ev.button.button);
            if (rk < 0) {
                return false;
            }
            RawInputEvent out{};
            out.type = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                           ? RawInputEvent::Type::MouseButtonDown
                           : RawInputEvent::Type::MouseButtonUp;
            out.raw_key = rk;
            out.x = std::clamp(ev.button.x / vw, 0.0f, 1.0f);
            out.y = std::clamp(ev.button.y / vh, 0.0f, 1.0f);
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            if (ev.wheel.which == SDL_TOUCH_MOUSEID) {
                return false;
            }
            RawInputEvent out{};
            out.type = RawInputEvent::Type::MouseWheel;
            // Undo "natural" scrolling so wheel-up always means `Scroll Up`.
            out.dy = (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) ? -ev.wheel.y : ev.wheel.y;
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            if (ev.motion.which == SDL_TOUCH_MOUSEID) {
                return false;
            }
            RawInputEvent out{};
            out.type = RawInputEvent::Type::MouseMotion;
            out.x = std::clamp(ev.motion.x / vw, 0.0f, 1.0f);
            out.y = std::clamp(ev.motion.y / vh, 0.0f, 1.0f);
            out.dx = ev.motion.xrel / vw;
            out.dy = ev.motion.yrel / vh;
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_GAMEPAD_ADDED: {
            const SDL_JoystickID jid = ev.gdevice.which;
            if (open_pads_.find(jid) != open_pads_.end()) {
                return false;
            }
            SDL_Gamepad* pad = SDL_OpenGamepad(jid);
            if (pad == nullptr) {
                // Never report a controller that will not deliver input (it would
                // hide the touch overlay for nothing).
                return false;
            }
            open_pads_.emplace(jid, pad);
            if (gyro_enabled_) {
                set_pad_gyro_locked(jid, pad, true);
            }
            RawInputEvent out{};
            out.type = RawInputEvent::Type::GamepadConnected;
            out.device_id = static_cast<std::uint64_t>(jid);
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_GAMEPAD_REMOVED: {
            const SDL_JoystickID jid = ev.gdevice.which;
            const auto it = open_pads_.find(jid);
            if (it != open_pads_.end()) {
                if (it->second != nullptr) {
                    SDL_CloseGamepad(it->second);
                }
                open_pads_.erase(it);
            }
            pad_axes_.erase(jid);
            gyro_pads_.erase(jid);
            RawInputEvent out{};
            out.type = RawInputEvent::Type::GamepadDisconnected;
            out.device_id = static_cast<std::uint64_t>(jid);
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            const std::int32_t rk = sdl_gamepad_button_to_raw_key(ev.gbutton.button);
            if (rk < 0) {
                return false;
            }
            RawInputEvent out{};
            out.type = (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
                           ? RawInputEvent::Type::GamepadButtonDown
                           : RawInputEvent::Type::GamepadButtonUp;
            out.raw_key = rk;
            out.device_id = static_cast<std::uint64_t>(ev.gbutton.which);
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            const SDL_JoystickID jid = ev.gaxis.which;
            AxisSnapshot& ax = pad_axes_[jid];
            switch (ev.gaxis.axis) {
            case SDL_GAMEPAD_AXIS_LEFTX:
                ax.lx = normalize_sdl_signed_axis(ev.gaxis.value);
                break;
            case SDL_GAMEPAD_AXIS_LEFTY:
                ax.ly = normalize_sdl_signed_axis(ev.gaxis.value);
                break;
            case SDL_GAMEPAD_AXIS_RIGHTX:
                ax.rx = normalize_sdl_signed_axis(ev.gaxis.value);
                break;
            case SDL_GAMEPAD_AXIS_RIGHTY:
                ax.ry = normalize_sdl_signed_axis(ev.gaxis.value);
                break;
            case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
                ax.lt = normalize_sdl_trigger(ev.gaxis.value);
                break;
            case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
                ax.rt = normalize_sdl_trigger(ev.gaxis.value);
                break;
            default:
                return false;
            }
            RawInputEvent out{};
            out.type = RawInputEvent::Type::GamepadAxis;
            out.device_id = static_cast<std::uint64_t>(jid);
            out.left_x = ax.lx;
            out.left_y = ax.ly;
            out.right_x = ax.rx;
            out.right_y = ax.ry;
            out.left_trigger = ax.lt;
            out.right_trigger = ax.rt;
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_MOTION:
        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED: {
            if (ev.tfinger.touchID == SDL_MOUSE_TOUCHID) {
                // Synthesized from a real mouse; it already arrived as mouse events.
                return false;
            }
            RawInputEvent out{};
            if (ev.type == SDL_EVENT_FINGER_DOWN) {
                out.type = RawInputEvent::Type::TouchDown;
            } else if (ev.type == SDL_EVENT_FINGER_MOTION) {
                out.type = RawInputEvent::Type::TouchMove;
            } else if (ev.type == SDL_EVENT_FINGER_UP) {
                out.type = RawInputEvent::Type::TouchUp;
            } else {
                // System gesture / palm rejection took the finger: no UP will follow.
                out.type = RawInputEvent::Type::TouchCancel;
            }
            out.finger_id = static_cast<std::int64_t>(ev.tfinger.fingerID);
            out.x = std::clamp(ev.tfinger.x, 0.0f, 1.0f);
            out.y = std::clamp(ev.tfinger.y, 0.0f, 1.0f);
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_DID_ENTER_BACKGROUND: {
            // Key / button UP events for anything held now go elsewhere.
            RawInputEvent out{};
            out.type = RawInputEvent::Type::FocusLost;
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_SENSOR_UPDATE: {
            // A controller gyro, when present, is what the player aims with; mixing in
            // the phone's own sensor would average two unrelated motions.
            if (!gyro_enabled_ || open_gyro_ == nullptr || ev.sensor.which != gyro_id_ ||
                !gyro_pads_.empty()) {
                return false;
            }
            // SDL3 reports rad/s CCW in the device's natural orientation and does not
            // rotate the axes with the screen, so landscape needs the remap.
            const GyroLookRates rates =
                gyro_look_rates(rotation_, ev.sensor.data[0], ev.sensor.data[1]);
            RawInputEvent out{};
            out.type = RawInputEvent::Type::GyroSample;
            out.dx = rates.yaw_right;
            out.dy = rates.pitch_up;
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
            if (!gyro_enabled_ || ev.gsensor.sensor != SDL_SENSOR_GYRO ||
                gyro_pads_.find(ev.gsensor.which) == gyro_pads_.end()) {
                return false;
            }
            // Controllers report in their own "held in front of you" frame.
            const GyroLookRates rates =
                gyro_look_rates(ScreenRotation::Deg0, ev.gsensor.data[0], ev.gsensor.data[1]);
            RawInputEvent out{};
            out.type = RawInputEvent::Type::GyroSample;
            out.device_id = static_cast<std::uint64_t>(ev.gsensor.which);
            out.dx = rates.yaw_right;
            out.dy = rates.pitch_up;
            queue_.push_back(out);
            return true;
        }
        case SDL_EVENT_DISPLAY_ORIENTATION:
            refresh_rotation_locked();
            return false;
        default:
            return false;
        }
    }

    void poll_events(std::vector<RawInputEvent>& out_events) override {
        std::lock_guard<std::mutex> lock(mutex_);
        out_events.swap(queue_);
        queue_.clear();
    }

    [[nodiscard]] const char* backend_name() const noexcept override { return "sdl3"; }

private:
    // Screen rotation relative to the natural orientation, which is the frame the
    // device gyro reports in (a natural-landscape tablet at rest is `Deg0`).
    void refresh_rotation_locked() noexcept {
        const SDL_DisplayID display = SDL_GetPrimaryDisplay();
        if (display == 0) {
            return;
        }
        const int current = sdl_orientation_degrees(SDL_GetCurrentDisplayOrientation(display));
        const int natural = sdl_orientation_degrees(SDL_GetNaturalDisplayOrientation(display));
        if (current < 0 || natural < 0) {
            return;
        }
        rotation_ = screen_rotation_from_degrees(current - natural);
    }

    void set_pad_gyro_locked(SDL_JoystickID jid, SDL_Gamepad* pad, bool enabled) noexcept {
        if (pad == nullptr || !SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
            return;
        }
        if (SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, enabled) && enabled) {
            gyro_pads_.insert(jid);
        } else {
            gyro_pads_.erase(jid);
        }
    }

    void open_gyro_locked() noexcept {
        if (open_gyro_ != nullptr) {
            return;
        }
        if ((SDL_WasInit(SDL_INIT_SENSOR) & SDL_INIT_SENSOR) == 0U) {
            if (!SDL_InitSubSystem(SDL_INIT_SENSOR)) {
                return;
            }
            owns_sensor_subsys_ = true;
        }
        int scount = 0;
        if (SDL_SensorID* sensors = SDL_GetSensors(&scount); sensors != nullptr) {
            for (int i = 0; i < scount; ++i) {
                if (SDL_GetSensorTypeForID(sensors[i]) == SDL_SENSOR_GYRO) {
                    if (SDL_Sensor* s = SDL_OpenSensor(sensors[i]); s != nullptr) {
                        open_gyro_ = s;
                        gyro_id_ = sensors[i];
                        break;
                    }
                }
            }
            SDL_free(sensors);
        }
    }

    void close_gyro_locked() noexcept {
        if (open_gyro_ != nullptr) {
            SDL_CloseSensor(open_gyro_);
            open_gyro_ = nullptr;
            gyro_id_ = 0;
        }
        if (owns_sensor_subsys_) {
            SDL_QuitSubSystem(SDL_INIT_SENSOR);
            owns_sensor_subsys_ = false;
        }
    }
    struct AxisSnapshot {
        float lx = 0.0f;
        float ly = 0.0f;
        float rx = 0.0f;
        float ry = 0.0f;
        float lt = 0.0f;
        float rt = 0.0f;
    };

    mutable std::mutex mutex_{};
    bool owns_gamepad_subsys_ = false;
    bool owns_sensor_subsys_ = false;
    bool gyro_enabled_ = false;
    int viewport_w_ = 1;
    int viewport_h_ = 1;
    std::vector<RawInputEvent> queue_{};
    std::unordered_map<SDL_JoystickID, SDL_Gamepad*> open_pads_{};
    std::unordered_map<SDL_JoystickID, AxisSnapshot> pad_axes_{};
    SDL_Sensor* open_gyro_ = nullptr;
    SDL_SensorID gyro_id_ = 0;
    ScreenRotation rotation_ = ScreenRotation::Deg0;
    std::unordered_set<SDL_JoystickID> gyro_pads_{};  // pads streaming gyro samples
};

}  // namespace

std::unique_ptr<SdlInputBackend> create_sdl_input_backend() {
    return std::make_unique<SdlInputBackendImpl>();
}

}  // namespace wse::platform
