// Procedural 2D touch-overlay renderer producing untextured `GeometryFrame` batches
// (`docs/PLATFORM_PLAN.md`, P11.3).
//
// Port infrastructure: emits alpha-blended triangles (`texture = 0`) using short-side
// widget sizing, safe-area clamping, per-widget opacity, and brighter pressed state.
// Shapes are generated procedurally (circle triangle fans, rounded rectangles, radial
// sector rings, and triangle arrow glyphs) with no texture assets.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "wse/platform/input.h"
#include "wse/platform/rhi_geometry.h"

namespace wse::platform {

struct TouchOverlayRenderParams {
    std::uint32_t viewport_width = 0U;
    std::uint32_t viewport_height = 0U;
    InputContext context = InputContext::Mission;
    SafeAreaInsets safe_area{};
    const InputFrameState* frame_state = nullptr;
    bool gamepad_hidden = false;
};

namespace touch_render_detail {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;

struct ColorRgba {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct SafePixelBounds {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

[[nodiscard]] inline SafePixelBounds compute_safe_pixel_bounds(
    const SafeAreaInsets& insets,
    float w_px,
    float h_px) noexcept {
    const auto nonneg = [](float v) noexcept {
        return (std::isfinite(v) && v > 0.0f) ? v : 0.0f;
    };
    float l = std::min(nonneg(insets.left), 0.98f);
    float r = std::min(nonneg(insets.right), 0.98f);
    if (l + r > 0.98f) {
        const float k = 0.98f / (l + r);
        l *= k;
        r *= k;
    }
    float t = std::min(nonneg(insets.top), 0.98f);
    float b = std::min(nonneg(insets.bottom), 0.98f);
    if (t + b > 0.98f) {
        const float k = 0.98f / (t + b);
        t *= k;
        b *= k;
    }
    return SafePixelBounds{l * w_px, t * h_px, (1.0f - r) * w_px, (1.0f - b) * h_px};
}

class OverlayBatchWriter {
public:
    OverlayBatchWriter(GeometryFrame& frame,
                       std::size_t base_vertex,
                       const SafePixelBounds& safe) noexcept
        : frame_(frame), base_vertex_(base_vertex), safe_(safe) {}

    [[nodiscard]] std::int32_t push_vertex(float x, float y, const ColorRgba& c) {
        GeometryVertex gv{};
        gv.x = std::clamp(x, safe_.x0, safe_.x1);
        gv.y = std::clamp(y, safe_.y0, safe_.y1);
        gv.r = std::clamp(c.r, 0.0f, 1.0f);
        gv.g = std::clamp(c.g, 0.0f, 1.0f);
        gv.b = std::clamp(c.b, 0.0f, 1.0f);
        gv.a = std::clamp(c.a, 0.0f, 1.0f);
        gv.u = 0.0f;
        gv.v = 0.0f;
        const auto idx = static_cast<std::int32_t>(frame_.vertices.size() - base_vertex_);
        frame_.vertices.push_back(gv);
        return idx;
    }

    void push_tri_indices(std::int32_t a, std::int32_t b, std::int32_t c) {
        frame_.indices.push_back(a);
        frame_.indices.push_back(b);
        frame_.indices.push_back(c);
    }

    void triangle(float x0, float y0,
                  float x1, float y1,
                  float x2, float y2,
                  const ColorRgba& c) {
        const std::int32_t i0 = push_vertex(x0, y0, c);
        const std::int32_t i1 = push_vertex(x1, y1, c);
        const std::int32_t i2 = push_vertex(x2, y2, c);
        push_tri_indices(i0, i1, i2);
    }

    void quad(float x0, float y0, float x1, float y1, const ColorRgba& c) {
        const std::int32_t i0 = push_vertex(x0, y0, c);
        const std::int32_t i1 = push_vertex(x1, y0, c);
        const std::int32_t i2 = push_vertex(x1, y1, c);
        const std::int32_t i3 = push_vertex(x0, y1, c);
        push_tri_indices(i0, i1, i2);
        push_tri_indices(i0, i2, i3);
    }

    // Triangle fan centered at `(cx, cy)` with `segments` rim vertices.
    void circle_fan(float cx, float cy, float radius, int segments, const ColorRgba& c) {
        if (radius <= 0.0f || segments < 3) {
            return;
        }
        const std::int32_t center = push_vertex(cx, cy, c);
        const std::int32_t first_rim = static_cast<std::int32_t>(frame_.vertices.size() - base_vertex_);
        for (int i = 0; i < segments; ++i) {
            const float ang = (kTwoPi * static_cast<float>(i)) / static_cast<float>(segments);
            (void)push_vertex(cx + radius * std::cos(ang), cy + radius * std::sin(ang), c);
        }
        for (int i = 0; i < segments; ++i) {
            const int next = (i + 1) % segments;
            push_tri_indices(center, first_rim + i, first_rim + next);
        }
    }

    // Annular sector from `angle_start` to `angle_end` (0 rad = North/-Y, clockwise +X).
    void ring_arc(float cx,
                  float cy,
                  float r_inner,
                  float r_outer,
                  float angle_start,
                  float angle_end,
                  int segments,
                  const ColorRgba& c) {
        if (r_outer <= r_inner || r_inner < 0.0f || segments < 1) {
            return;
        }
        const std::int32_t base = static_cast<std::int32_t>(frame_.vertices.size() - base_vertex_);
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float ang = angle_start + (angle_end - angle_start) * t;
            const float sx = std::sin(ang);
            const float sy = -std::cos(ang);
            (void)push_vertex(cx + r_inner * sx, cy + r_inner * sy, c);
            (void)push_vertex(cx + r_outer * sx, cy + r_outer * sy, c);
        }
        for (int i = 0; i < segments; ++i) {
            const std::int32_t i0 = base + i * 2;
            const std::int32_t o0 = i0 + 1;
            const std::int32_t i1 = i0 + 2;
            const std::int32_t o1 = i0 + 3;
            push_tri_indices(i0, o0, o1);
            push_tri_indices(i0, o1, i1);
        }
    }

    void rounded_rect(float x0,
                      float y0,
                      float x1,
                      float y1,
                      float corner_radius,
                      int corner_segments,
                      const ColorRgba& c) {
        const float w = x1 - x0;
        const float h = y1 - y0;
        if (w <= 0.0f || h <= 0.0f) {
            return;
        }
        const float cr = std::clamp(corner_radius, 0.0f, std::min(w, h) * 0.5f);
        if (cr <= 0.5f || corner_segments < 1) {
            quad(x0, y0, x1, y1, c);
            return;
        }
        const std::int32_t center = push_vertex(0.5f * (x0 + x1), 0.5f * (y0 + y1), c);
        const std::int32_t first_rim = static_cast<std::int32_t>(frame_.vertices.size() - base_vertex_);
        const std::array<std::pair<float, float>, 4> centers = {{
            {x1 - cr, y0 + cr},  // top-right (-pi/2 .. 0)
            {x1 - cr, y1 - cr},  // bottom-right (0 .. pi/2)
            {x0 + cr, y1 - cr},  // bottom-left (pi/2 .. pi)
            {x0 + cr, y0 + cr},  // top-left (pi .. 3pi/2)
        }};
        const std::array<float, 4> start_angles = {
            -0.5f * kPi,
            0.0f,
            0.5f * kPi,
            kPi,
        };
        int total_rim = 0;
        for (std::size_t q = 0; q < 4U; ++q) {
            for (int s = 0; s <= corner_segments; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(corner_segments);
                const float ang = start_angles[q] + (0.5f * kPi) * t;
                (void)push_vertex(centers[q].first + cr * std::cos(ang),
                                  centers[q].second + cr * std::sin(ang), c);
                ++total_rim;
            }
        }
        for (int i = 0; i < total_rim; ++i) {
            const int next = (i + 1) % total_rim;
            push_tri_indices(center, first_rim + i, first_rim + next);
        }
    }

    void rounded_rect_outline(float x0,
                              float y0,
                              float x1,
                              float y1,
                              float corner_radius,
                              float thickness,
                              int corner_segments,
                              const ColorRgba& c) {
        const float w = x1 - x0;
        const float h = y1 - y0;
        if (w <= 0.0f || h <= 0.0f || thickness <= 0.0f) {
            return;
        }
        const float r_out = std::clamp(corner_radius, 0.0f, std::min(w, h) * 0.5f);
        const float th = std::min(thickness, std::min(w, h) * 0.4f);
        const float r_in = std::max(0.0f, r_out - th);
        const std::array<std::pair<float, float>, 4> centers = {{
            {x1 - r_out, y0 + r_out},
            {x1 - r_out, y1 - r_out},
            {x0 + r_out, y1 - r_out},
            {x0 + r_out, y0 + r_out},
        }};
        const std::array<float, 4> start_angles = {
            -0.5f * kPi,
            0.0f,
            0.5f * kPi,
            kPi,
        };
        const std::int32_t base = static_cast<std::int32_t>(frame_.vertices.size() - base_vertex_);
        int ring_pts = 0;
        const int segs = std::max(corner_segments, 1);
        for (std::size_t q = 0; q < 4U; ++q) {
            for (int s = 0; s <= segs; ++s) {
                const float t = static_cast<float>(s) / static_cast<float>(segs);
                const float ang = start_angles[q] + (0.5f * kPi) * t;
                const float ca = std::cos(ang);
                const float sa = std::sin(ang);
                (void)push_vertex(centers[q].first + r_in * ca, centers[q].second + r_in * sa, c);
                (void)push_vertex(centers[q].first + r_out * ca, centers[q].second + r_out * sa, c);
                ++ring_pts;
            }
        }
        for (int i = 0; i < ring_pts; ++i) {
            const int next = (i + 1) % ring_pts;
            const std::int32_t i0 = base + i * 2;
            const std::int32_t o0 = i0 + 1;
            const std::int32_t i1 = base + next * 2;
            const std::int32_t o1 = i1 + 1;
            push_tri_indices(i0, o0, o1);
            push_tri_indices(i0, o1, i1);
        }
    }

    void arrow_glyph(float cx, float cy, float size, TouchCombatDirection dir, const ColorRgba& c) {
        switch (dir) {
        case TouchCombatDirection::Up:
            triangle(cx, cy - size, cx + size * 0.85f, cy + size * 0.65f,
                     cx - size * 0.85f, cy + size * 0.65f, c);
            break;
        case TouchCombatDirection::Down:
            triangle(cx, cy + size, cx - size * 0.85f, cy - size * 0.65f,
                     cx + size * 0.85f, cy - size * 0.65f, c);
            break;
        case TouchCombatDirection::Left:
            triangle(cx - size, cy, cx + size * 0.65f, cy - size * 0.85f,
                     cx + size * 0.65f, cy + size * 0.85f, c);
            break;
        case TouchCombatDirection::Right:
            triangle(cx + size, cy, cx - size * 0.65f, cy + size * 0.85f,
                     cx - size * 0.65f, cy - size * 0.85f, c);
            break;
        case TouchCombatDirection::None:
        default:
            break;
        }
    }

private:
    GeometryFrame& frame_;
    std::size_t base_vertex_ = 0U;
    SafePixelBounds safe_{};
};

}  // namespace touch_render_detail

// Appends untextured alpha-blended triangles (`texture = 0, blend = BlendMode::alpha`)
// for every visible widget of `profile` in `params.context`, inside `params.safe_area`.
// Returns the number of triangles emitted.
inline std::uint32_t append_touch_overlay_geometry(
    const TouchOverlayProfile& profile,
    const TouchOverlayRenderParams& params,
    GeometryFrame& out_frame) {
    const std::uint32_t vw = (params.viewport_width > 0U) ? params.viewport_width : out_frame.width;
    const std::uint32_t vh = (params.viewport_height > 0U) ? params.viewport_height : out_frame.height;
    if (vw == 0U || vh == 0U || !profile.touch_overlay_enabled || params.gamepad_hidden ||
        !context_allows_touch_overlay(params.context)) {
        return 0U;
    }
    if (out_frame.width == 0U) {
        out_frame.width = vw;
    }
    if (out_frame.height == 0U) {
        out_frame.height = vh;
    }

    const float fw = static_cast<float>(vw);
    const float fh = static_cast<float>(vh);
    const float aspect = sanitize_viewport_aspect(fw / fh);
    TouchOverlayProfile clamped = profile;
    apply_safe_area(clamped, params.safe_area, aspect);
    const touch_render_detail::SafePixelBounds safe_px =
        touch_render_detail::compute_safe_pixel_bounds(params.safe_area, fw, fh);

    const std::size_t base_vertex = out_frame.vertices.size();
    const std::size_t base_index = out_frame.indices.size();
    touch_render_detail::OverlayBatchWriter w(out_frame, base_vertex, safe_px);

    const InputFrameState* st = params.frame_state;

    for (std::size_t i = 0; i < kNumTouchWidgets; ++i) {
        const auto id = static_cast<TouchWidgetId>(i);
        const TouchWidgetLayout& lay = clamped.widgets[i];
        if (!is_touch_widget_active(id, params.context, lay, true)) {
            continue;
        }
        const bool pressed = (st != nullptr) ? st->widget_pressed[i] : false;
        const float op = std::clamp(lay.opacity, 0.0f, 1.0f);

        const touch_render_detail::ColorRgba fill =
            pressed ? touch_render_detail::ColorRgba{0.48f, 0.62f, 0.80f, 0.65f * op}
                    : touch_render_detail::ColorRgba{0.18f, 0.22f, 0.28f, 0.45f * op};
        const touch_render_detail::ColorRgba border =
            pressed ? touch_render_detail::ColorRgba{0.92f, 0.96f, 1.00f, 0.95f * op}
                    : touch_render_detail::ColorRgba{0.56f, 0.64f, 0.74f, 0.78f * op};
        const touch_render_detail::ColorRgba glyph =
            pressed ? touch_render_detail::ColorRgba{1.00f, 1.00f, 1.00f, 1.00f * op}
                    : touch_render_detail::ColorRgba{0.84f, 0.88f, 0.94f, 0.90f * op};

        const float cx = lay.cx * fw;
        const float cy = lay.cy * fh;
        const float hw = lay.norm_half_width(aspect) * fw;
        const float hh = lay.norm_half_height(aspect) * fh;
        const float min_side = std::min(hw, hh);
        const float x0 = cx - hw;
        const float y0 = cy - hh;
        const float x1 = cx + hw;
        const float y1 = cy + hh;
        const float border_th = std::max(1.5f, min_side * 0.07f);

        if (id == TouchWidgetId::LookArea) {
            const touch_render_detail::ColorRgba look_fill =
                pressed ? touch_render_detail::ColorRgba{0.38f, 0.48f, 0.62f, 0.32f * op}
                        : touch_render_detail::ColorRgba{0.14f, 0.18f, 0.22f, 0.18f * op};
            const touch_render_detail::ColorRgba look_border =
                pressed ? touch_render_detail::ColorRgba{0.72f, 0.82f, 0.94f, 0.55f * op}
                        : touch_render_detail::ColorRgba{0.42f, 0.48f, 0.56f, 0.35f * op};
            w.rounded_rect(x0, y0, x1, y1, min_side * 0.12f, 4, look_fill);
            w.rounded_rect_outline(x0, y0, x1, y1, min_side * 0.12f, 1.5f, 4, look_border);
            continue;
        }

        if (id == TouchWidgetId::MoveStick) {
            const float r = lay.circle_radius(aspect) * fh;
            w.circle_fan(cx, cy, r, 24, fill);
            w.ring_arc(cx, cy, std::max(1.0f, r - border_th), r, 0.0f,
                       touch_render_detail::kTwoPi, 24, border);
            const float arrow_off = r * 0.66f;
            const float arrow_sz = r * 0.14f;
            w.arrow_glyph(cx, cy - arrow_off, arrow_sz, TouchCombatDirection::Up, glyph);
            w.arrow_glyph(cx, cy + arrow_off, arrow_sz, TouchCombatDirection::Down, glyph);
            w.arrow_glyph(cx - arrow_off, cy, arrow_sz, TouchCombatDirection::Left, glyph);
            w.arrow_glyph(cx + arrow_off, cy, arrow_sz, TouchCombatDirection::Right, glyph);

            const float kx = (st != nullptr) ? std::clamp(st->stick_knob_x, -1.0f, 1.0f) : 0.0f;
            const float ky = (st != nullptr) ? std::clamp(st->stick_knob_y, -1.0f, 1.0f) : 0.0f;
            const float knob_r = r * 0.36f;
            const float travel = std::max(0.0f, r - knob_r);
            w.circle_fan(cx + kx * travel, cy - ky * travel, knob_r, 20, glyph);
            continue;
        }

        if (id == TouchWidgetId::CommandRadial) {
            const float r = lay.circle_radius(aspect) * fh;
            const bool open = (st != nullptr && st->radial_open && st->radial_level >= 1);
            if (!open) {
                w.circle_fan(cx, cy, r, 24, fill);
                w.ring_arc(cx, cy, std::max(1.0f, r - border_th), r, 0.0f,
                           touch_render_detail::kTwoPi, 24, border);
                const float bar_w = r * 0.55f;
                const float bar_h = r * 0.09f;
                w.quad(cx - bar_w, cy - r * 0.28f - bar_h, cx + bar_w, cy - r * 0.28f + bar_h,
                       glyph);
                w.quad(cx - bar_w, cy - bar_h, cx + bar_w, cy + bar_h, glyph);
                w.quad(cx - bar_w, cy + r * 0.28f - bar_h, cx + bar_w, cy + r * 0.28f + bar_h,
                       glyph);
            } else {
                const float max_avail_r =
                    std::max(r, std::min({cx - safe_px.x0, safe_px.x1 - cx, cy - safe_px.y0,
                                          safe_px.y1 - cy}));
                const float r1_in = r * 0.24f;
                const float r1_out = std::min(r * 0.95f, max_avail_r);
                const float sector_gap = 0.04f;
                for (int q = 0; q < 4; ++q) {
                    const float a_center = static_cast<float>(q) * (0.5f * touch_render_detail::kPi);
                    const float a0 = a_center - (0.25f * touch_render_detail::kPi - sector_gap);
                    const float a1 = a_center + (0.25f * touch_render_detail::kPi - sector_gap);
                    const bool lit = (st->radial_group == q);
                    const touch_render_detail::ColorRgba seg_col =
                        lit ? touch_render_detail::ColorRgba{0.72f, 0.84f, 0.98f, 0.92f * op}
                            : fill;
                    w.ring_arc(cx, cy, r1_in, r1_out, a0, a1, 6, seg_col);
                }
                if (st->radial_level >= 2) {
                    const float r2_in = std::min(r * 1.05f, max_avail_r * 0.65f);
                    const float r2_out = std::min(r * 1.90f, max_avail_r);
                    if (r2_out > r2_in + 2.0f) {
                        for (int s = 0; s < 8; ++s) {
                            const float a_center =
                                static_cast<float>(s) * (0.25f * touch_render_detail::kPi);
                            const float a0 =
                                a_center - (0.125f * touch_render_detail::kPi - sector_gap * 0.5f);
                            const float a1 =
                                a_center + (0.125f * touch_render_detail::kPi - sector_gap * 0.5f);
                            const bool lit = (st->radial_sector == s);
                            const touch_render_detail::ColorRgba seg_col =
                                lit ? touch_render_detail::ColorRgba{0.82f, 0.90f, 1.00f,
                                                                     0.95f * op}
                                    : fill;
                            w.ring_arc(cx, cy, r2_in, r2_out, a0, a1, 4, seg_col);
                        }
                    }
                }
            }
            continue;
        }

        // Rectangular / rounded-rect widgets and context buttons.
        const float cr = min_side * 0.25f;
        w.rounded_rect(x0, y0, x1, y1, cr, 4, fill);
        w.rounded_rect_outline(x0, y0, x1, y1, cr, border_th, 4, border);

        const float g_sz = min_side * 0.42f;
        switch (id) {
        case TouchWidgetId::AttackZone:
        case TouchWidgetId::BlockZone: {
            const TouchCombatDirection active_dir =
                (st != nullptr)
                    ? ((id == TouchWidgetId::AttackZone)
                           ? (st->attack_active ? st->attack_dir : TouchCombatDirection::None)
                           : (st->block_active ? st->block_dir : TouchCombatDirection::None))
                    : TouchCombatDirection::None;
            const float off = min_side * 0.52f;
            const float arr = min_side * 0.20f;
            const touch_render_detail::ColorRgba lit{1.0f, 0.95f, 0.55f, 1.0f * op};
            for (const TouchCombatDirection dir :
                 {TouchCombatDirection::Up, TouchCombatDirection::Down,
                  TouchCombatDirection::Left, TouchCombatDirection::Right}) {
                const touch_render_detail::ColorRgba& ac = (active_dir == dir) ? lit : glyph;
                const float ox = (dir == TouchCombatDirection::Right)  ? off
                                 : (dir == TouchCombatDirection::Left) ? -off
                                                                       : 0.0f;
                const float oy = (dir == TouchCombatDirection::Down) ? off
                                 : (dir == TouchCombatDirection::Up) ? -off
                                                                     : 0.0f;
                w.arrow_glyph(cx + ox, cy + oy, arr, dir, ac);
            }
            w.circle_fan(cx, cy, min_side * 0.14f, 12, glyph);
            break;
        }
        case TouchWidgetId::MapCenter:
            w.triangle(cx, cy - g_sz, cx + g_sz, cy, cx, cy + g_sz, glyph);
            w.triangle(cx, cy - g_sz, cx, cy + g_sz, cx - g_sz, cy, glyph);
            break;
        case TouchWidgetId::MapFollow: {
            const bool active = (st != nullptr && st->map_follow);
            const touch_render_detail::ColorRgba col =
                active ? touch_render_detail::ColorRgba{0.90f, 0.98f, 0.78f, 1.0f * op} : glyph;
            w.arrow_glyph(cx, cy, g_sz * 0.85f, TouchCombatDirection::Up, col);
            break;
        }
        case TouchWidgetId::MapPanMode: {
            const bool active = (st != nullptr && st->map_pan_mode);
            const touch_render_detail::ColorRgba col =
                active ? touch_render_detail::ColorRgba{0.90f, 0.98f, 0.78f, 1.0f * op} : glyph;
            const float p_off = min_side * 0.44f;
            const float p_sz = min_side * 0.22f;
            w.arrow_glyph(cx, cy - p_off, p_sz, TouchCombatDirection::Up, col);
            w.arrow_glyph(cx, cy + p_off, p_sz, TouchCombatDirection::Down, col);
            w.arrow_glyph(cx - p_off, cy, p_sz, TouchCombatDirection::Left, col);
            w.arrow_glyph(cx + p_off, cy, p_sz, TouchCombatDirection::Right, col);
            break;
        }
        case TouchWidgetId::MapWait: {
            const bool paused = (st != nullptr && st->map_paused);
            if (paused) {
                w.arrow_glyph(cx, cy, g_sz * 0.85f, TouchCombatDirection::Right, glyph);
            } else {
                const float bw = g_sz * 0.26f;
                const float bh = g_sz * 0.85f;
                w.quad(cx - g_sz * 0.45f, cy - bh, cx - g_sz * 0.45f + bw * 2.0f, cy + bh, glyph);
                w.quad(cx + g_sz * 0.45f - bw * 2.0f, cy - bh, cx + g_sz * 0.45f, cy + bh, glyph);
            }
            break;
        }
        case TouchWidgetId::DialogUp:
            w.arrow_glyph(cx, cy, g_sz * 0.85f, TouchCombatDirection::Up, glyph);
            break;
        case TouchWidgetId::DialogSelect:
            w.arrow_glyph(cx, cy, g_sz * 0.85f, TouchCombatDirection::Right, glyph);
            break;
        case TouchWidgetId::DialogDown:
            w.arrow_glyph(cx, cy, g_sz * 0.85f, TouchCombatDirection::Down, glyph);
            break;
        case TouchWidgetId::MoveAll:
            w.arrow_glyph(cx - g_sz * 0.35f, cy, g_sz * 0.65f, TouchCombatDirection::Right, glyph);
            w.arrow_glyph(cx + g_sz * 0.35f, cy, g_sz * 0.65f, TouchCombatDirection::Right, glyph);
            break;
        case TouchWidgetId::MoveOne:
            w.arrow_glyph(cx, cy, g_sz * 0.75f, TouchCombatDirection::Right, glyph);
            break;
        case TouchWidgetId::Spare1:
        case TouchWidgetId::Spare2:
        case TouchWidgetId::Spare3:
        case TouchWidgetId::Spare4: {
            const int count =
                1 + static_cast<int>(id) - static_cast<int>(TouchWidgetId::Spare1);
            const float pip = min_side * 0.14f;
            const float step = pip * 2.6f;
            const float start_x = cx - 0.5f * step * static_cast<float>(count - 1);
            for (int p = 0; p < count; ++p) {
                const float px = start_x + step * static_cast<float>(p);
                w.quad(px - pip, cy - pip, px + pip, cy + pip, glyph);
            }
            break;
        }
        default:
            break;
        }
    }

    const std::size_t new_indices = out_frame.indices.size() - base_index;
    if (new_indices == 0U) {
        return 0U;
    }
    GeometryBatch batch{};
    batch.texture = 0U;
    batch.blend = BlendMode::alpha;
    batch.first_vertex = static_cast<std::uint32_t>(base_vertex);
    batch.vertex_count = static_cast<std::uint32_t>(out_frame.vertices.size() - base_vertex);
    batch.first_index = static_cast<std::uint32_t>(base_index);
    batch.index_count = static_cast<std::uint32_t>(new_indices);
    out_frame.batches.push_back(batch);
    return static_cast<std::uint32_t>(new_indices / 3U);
}

[[nodiscard]] inline GeometryFrame build_touch_overlay_frame(
    const TouchOverlayProfile& profile,
    const TouchOverlayRenderParams& params) {
    GeometryFrame frame{};
    frame.width = params.viewport_width;
    frame.height = params.viewport_height;
    (void)append_touch_overlay_geometry(profile, params, frame);
    return frame;
}

}  // namespace wse::platform
