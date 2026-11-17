// Per-frame UI glue for app shells (SDL / Android): platform input events -> `UiPointerInput`,
// then `UiSession::handle_input` + `UiSession::drive_frame` + `UiRhiRenderer::render_tree`.
//
// Port infrastructure (no single retail counterpart): retail feeds the same states through
// `rglWindowManager::frameMove` (`0x14032a0d0`); see `game::kUiStateEnter`.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "wse/core/encoding.h"
#include "wse/game/ui_runtime.h"
#include "wse/platform/input.h"
#include "wse/platform/rhi_ui.h"

namespace wse::platform {

// Collects one frame of `RawInputEvent`s (window-normalised, `y` down, as `sdl_input.cpp`
// produces them) into a `game::UiPointerInput` in Warband UI space (`x` in `[0, 1]`, `y` up in
// `[0, 0.75]`). The first touch finger acts as the left mouse button; a cancelled touch
// releases it without a click (the button disarms, as when the cursor leaves it). Escape,
// Backspace, and Enter also reach a focused text box as `\x1b` / `\b` / `\r`; typed text comes
// from the host's text events through `feed_text` (`SDL_EVENT_TEXT_INPUT`).
class UiInputAccumulator {
public:
    void feed(const RawInputEvent& ev) {
        switch (ev.type) {
        case RawInputEvent::Type::MouseMotion:
            set_position(ev.x, ev.y);
            break;
        case RawInputEvent::Type::MouseButtonDown:
            set_position(ev.x, ev.y);
            if (ev.raw_key == RawKey::kLeftMouse) {
                left_down_ = true;
                frame_.left_pressed = true;
            } else if (ev.raw_key == RawKey::kRightMouse) {
                frame_.right_pressed = true;
            } else if (ev.raw_key == RawKey::kMiddleMouse) {
                frame_.middle_pressed = true;
            }
            break;
        case RawInputEvent::Type::MouseButtonUp:
            set_position(ev.x, ev.y);
            if (ev.raw_key == RawKey::kLeftMouse) {
                left_down_ = false;
                frame_.left_released = true;
            }
            break;
        case RawInputEvent::Type::MouseWheel:
            frame_.wheel += ev.dy;
            break;
        case RawInputEvent::Type::TouchDown:
            if (touch_finger_ < 0) {
                touch_finger_ = ev.finger_id;
                set_position(ev.x, ev.y);
                left_down_ = true;
                frame_.left_pressed = true;
            }
            break;
        case RawInputEvent::Type::TouchMove:
            if (ev.finger_id == touch_finger_) {
                set_position(ev.x, ev.y);
            }
            break;
        case RawInputEvent::Type::TouchUp:
            if (ev.finger_id == touch_finger_) {
                set_position(ev.x, ev.y);
                touch_finger_ = -1;
                left_down_ = false;
                frame_.left_released = true;
            }
            break;
        case RawInputEvent::Type::TouchCancel:
            if (ev.finger_id == touch_finger_) {
                touch_finger_ = -1;
                left_down_ = false;
                // Move off-screen so the armed button sees a leave instead of a release.
                frame_.x = -1.0f;
                frame_.y = -1.0f;
            }
            break;
        case RawInputEvent::Type::FocusLost:
            touch_finger_ = -1;
            left_down_ = false;
            break;
        case RawInputEvent::Type::KeyDown:
            if (ev.raw_key == RawKey::kEscape) {
                frame_.escape_pressed = true;
                frame_.text_input.push_back(u'\x1b');
            } else if (ev.raw_key == RawKey::kBackSpace) {
                frame_.text_input.push_back(u'\b');
            } else if (ev.raw_key == RawKey::kEnter) {
                frame_.text_input.push_back(u'\r');
            }
            break;
        default:
            break;
        }
    }

    // UTF-8 text typed this frame (`SDL_EVENT_TEXT_INPUT::text`).
    void feed_text(std::string_view utf8) { frame_.text_input += core::wse_utf8_to_utf16(utf8); }

    // The frame's input; edges and text reset for the next frame, held state carries over.
    [[nodiscard]] game::UiPointerInput take(float time_s) {
        game::UiPointerInput out = frame_;
        out.left_down = left_down_;
        out.time_s = time_s;
        frame_ = game::UiPointerInput{};
        frame_.x = out.x;
        frame_.y = out.y;
        return out;
    }

private:
    void set_position(float nx, float ny) noexcept {
        frame_.x = nx * game::kUiScreenWidth;
        frame_.y = (1.0f - ny) * game::kUiScreenHeight;
    }

    game::UiPointerInput frame_{};
    bool left_down_ = false;
    std::int64_t touch_finger_ = -1;
};

struct UiFrameResult {
    std::vector<game::UiEvent> events{};
    game::UiSession::FrameReport report{};
    bool rendered = false;
};

// One UI frame: input first (so triggers see this frame's clicks), then the window queue /
// presentation step, then (when `renderer` and `font` are given) the widget tree is drawn into
// `out_rgba8`. `window_before_frame` is `world.m5.active_window` before `step_world_frame`.
inline UiFrameResult run_ui_frame(game::UiSession& session,
                                  game::WseWorldState& world,
                                  vm::VmContext& vm_ctx,
                                  const game::UiPointerInput& input,
                                  float dt_s,
                                  std::span<const game::LoadedPresentation> presentations,
                                  std::span<const game::LoadedMenu> menus,
                                  std::int32_t window_before_frame,
                                  UiRhiRenderer* renderer = nullptr,
                                  const asset::FontData* font = nullptr,
                                  const UiRenderConfig& cfg = UiRenderConfig{},
                                  std::vector<std::uint8_t>* out_rgba8 = nullptr) {
    UiFrameResult res{};
    res.events = session.handle_input(world, input, &vm_ctx, presentations);
    res.report = session.drive_frame(world, vm_ctx, dt_s, presentations, window_before_frame, menus);
    if (renderer != nullptr && font != nullptr && out_rgba8 != nullptr) {
        res.rendered = renderer->render_tree(session.root, *font, cfg, *out_rgba8);
    }
    return res;
}

}  // namespace wse::platform
