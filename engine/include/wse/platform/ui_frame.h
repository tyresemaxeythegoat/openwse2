// Per-frame UI glue for app shells (SDL / Android): platform input events -> `UiPointerInput`,
// then `UiSession::handle_input` + `UiSession::drive_frame` + `UiRhiRenderer::render_tree`.
//
// Port infrastructure (no single retail counterpart): retail feeds the same states through
// `rglWindowManager::frameMove` (`0x14032ae90`); see `game::kUiStateEnter`.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "wse/core/encoding.h"
#include "wse/game/ui_runtime.h"
#include "wse/platform/input.h"
#include "wse/platform/performance.h"
#include "wse/platform/rhi_ui.h"

namespace wse::platform {

struct UiViewportRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] friend bool operator==(const UiViewportRect&, const UiViewportRect&) = default;
};

// Largest centred rectangle inside `win_w x win_h` matching `mode` (`Stretch` fills the
// window; an empty window returns all zeros).
[[nodiscard]] inline UiViewportRect compute_ui_viewport(std::uint32_t win_w,
                                                        std::uint32_t win_h,
                                                        UiAspectMode mode) noexcept {
    if (win_w == 0U || win_h == 0U) {
        return UiViewportRect{};
    }
    if (mode == UiAspectMode::Stretch) {
        return UiViewportRect{0U, 0U, win_w, win_h};
    }
    const std::uint64_t num = (mode == UiAspectMode::Ratio16x9) ? 16ULL : 4ULL;
    const std::uint64_t den = (mode == UiAspectMode::Ratio16x9) ? 9ULL : 3ULL;
    UiViewportRect vp{};
    if (static_cast<std::uint64_t>(win_w) * den > static_cast<std::uint64_t>(win_h) * num) {
        const auto scaled_w = static_cast<std::uint32_t>(
            std::llround(static_cast<double>(win_h) * static_cast<double>(num) /
                         static_cast<double>(den)));
        vp.width = std::clamp<std::uint32_t>(scaled_w, 1U, win_w);
        vp.height = win_h;
    } else {
        const auto scaled_h = static_cast<std::uint32_t>(
            std::llround(static_cast<double>(win_w) * static_cast<double>(den) /
                         static_cast<double>(num)));
        vp.width = win_w;
        vp.height = std::clamp<std::uint32_t>(scaled_h, 1U, win_h);
    }
    vp.x = (win_w - vp.width) / 2U;
    vp.y = (win_h - vp.height) / 2U;
    return vp;
}

// Collects one frame of `RawInputEvent`s (window-normalised, `y` down, as `sdl_input.cpp`
// produces them) into a `game::UiPointerInput` in Warband UI space (`x` in `[0, 1]`, `y` up in
// `[0, 0.75]`). The first touch finger acts as the left mouse button; a cancelled touch
// releases it without a click (the button disarms, as when the cursor leaves it). Escape,
// Backspace, and Enter also reach a focused text box as `\x1b` / `\b` / `\r`; typed text comes
// from the host's text events through `feed_text` (`SDL_EVENT_TEXT_INPUT`).
class UiInputAccumulator {
public:
    // Maps subsequent (and already-buffered) window-normalised coordinates through `vp` inside
    // `win_w x win_h` without clamping, so points on letterbox bars land outside the UI canvas.
    void set_viewport(UiViewportRect vp, std::uint32_t win_w, std::uint32_t win_h) noexcept {
        viewport_ = vp;
        win_w_ = win_w;
        win_h_ = win_h;
        has_viewport_ = true;
        if (has_raw_pos_) {
            apply_raw_position();
        }
    }

    void feed(const RawInputEvent& ev) {
        switch (ev.type) {
        case RawInputEvent::Type::MouseMotion:
            set_position(ev.x, ev.y);
            frame_.touch = false;
            mouse_pointer_this_frame_ = true;
            break;
        case RawInputEvent::Type::MouseButtonDown:
            set_position(ev.x, ev.y);
            frame_.touch = false;
            mouse_pointer_this_frame_ = true;
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
            frame_.touch = false;
            mouse_pointer_this_frame_ = true;
            if (ev.raw_key == RawKey::kLeftMouse) {
                left_down_ = false;
                frame_.left_released = true;
            }
            break;
        case RawInputEvent::Type::MouseWheel:
            frame_.wheel += ev.dy;
            if (touch_finger_ < 0) {
                frame_.touch = false;
            }
            break;
        case RawInputEvent::Type::TouchDown:
            if (touch_finger_ < 0) {
                touch_finger_ = ev.finger_id;
                set_position(ev.x, ev.y);
                left_down_ = true;
                frame_.left_pressed = true;
                frame_.touch = true;
                mouse_pointer_this_frame_ = false;
            }
            break;
        case RawInputEvent::Type::TouchMove:
            if (ev.finger_id == touch_finger_) {
                set_position(ev.x, ev.y);
                frame_.touch = true;
                mouse_pointer_this_frame_ = false;
            }
            break;
        case RawInputEvent::Type::TouchUp:
            if (ev.finger_id == touch_finger_) {
                set_position(ev.x, ev.y);
                touch_finger_ = -1;
                left_down_ = false;
                frame_.left_released = true;
                frame_.touch = true;
                mouse_pointer_this_frame_ = false;
            }
            break;
        case RawInputEvent::Type::TouchCancel:
            if (ev.finger_id == touch_finger_) {
                touch_finger_ = -1;
                left_down_ = false;
                has_raw_pos_ = false;
                frame_.touch = false;
                // Move off-screen so the armed button sees a leave instead of a release.
                frame_.x = -1.0f;
                frame_.y = -1.0f;
            }
            break;
        case RawInputEvent::Type::FocusLost:
            touch_finger_ = -1;
            left_down_ = false;
            frame_.touch = false;
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
        out.touch = frame_.touch || (touch_finger_ >= 0 && !mouse_pointer_this_frame_);
        out.time_s = time_s;
        frame_ = game::UiPointerInput{};
        frame_.x = out.x;
        frame_.y = out.y;
        mouse_pointer_this_frame_ = false;
        return out;
    }

private:
    void set_position(float nx, float ny) noexcept {
        raw_nx_ = nx;
        raw_ny_ = ny;
        has_raw_pos_ = true;
        apply_raw_position();
    }

    void apply_raw_position() noexcept {
        float vx = raw_nx_;
        float vy = raw_ny_;
        if (has_viewport_) {
            if (win_w_ == 0U || win_h_ == 0U || viewport_.width == 0U || viewport_.height == 0U) {
                frame_.x = -1.0f;
                frame_.y = -1.0f;
                return;
            }
            const float px = raw_nx_ * static_cast<float>(win_w_);
            const float py = raw_ny_ * static_cast<float>(win_h_);
            vx = (px - static_cast<float>(viewport_.x)) / static_cast<float>(viewport_.width);
            vy = (py - static_cast<float>(viewport_.y)) / static_cast<float>(viewport_.height);
        }
        frame_.x = vx * game::kUiScreenWidth;
        frame_.y = (1.0f - vy) * game::kUiScreenHeight;
    }

    game::UiPointerInput frame_{};
    bool left_down_ = false;
    bool mouse_pointer_this_frame_ = false;
    std::int64_t touch_finger_ = -1;
    UiViewportRect viewport_{};
    std::uint32_t win_w_ = 0;
    std::uint32_t win_h_ = 0;
    bool has_viewport_ = false;
    bool has_raw_pos_ = false;
    float raw_nx_ = 0.0f;
    float raw_ny_ = 0.0f;
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
