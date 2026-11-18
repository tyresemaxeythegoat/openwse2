// Platform seam: P8 thermal throttling awareness, display frame-rate caps
// (30/60/90/120 Hz + uncapped), Vulkan/SDL present-mode selection, deterministic
// clock-driven frame pacing, and persistent performance settings
// (`docs/PLATFORM_PLAN.md`, slice P8).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations. Standard library only: keeps `<SDL3/SDL.h>`
// and `<jni.h>` out of portable engine headers.

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/core/json.h"
#include "wse/platform/clock.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

namespace perf_detail {

[[nodiscard]] constexpr std::string_view trim_ascii_ws(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace perf_detail

enum class FrameRateCap : std::uint8_t {
    Uncapped = 0,
    // Battery saver: also the rate `ThermalTier::Critical` falls back to.
    Fps30 = 30,
    Fps60 = 60,
    Fps90 = 90,
    Fps120 = 120,
};

[[nodiscard]] constexpr std::int32_t frame_rate_cap_hz(FrameRateCap cap) noexcept {
    switch (cap) {
    case FrameRateCap::Uncapped:
        return 0;
    case FrameRateCap::Fps30:
        return 30;
    case FrameRateCap::Fps60:
        return 60;
    case FrameRateCap::Fps90:
        return 90;
    case FrameRateCap::Fps120:
        return 120;
    }
    return 60;
}

[[nodiscard]] constexpr std::string_view frame_rate_cap_key(FrameRateCap cap) noexcept {
    switch (cap) {
    case FrameRateCap::Uncapped:
        return "uncapped";
    case FrameRateCap::Fps30:
        return "30";
    case FrameRateCap::Fps60:
        return "60";
    case FrameRateCap::Fps90:
        return "90";
    case FrameRateCap::Fps120:
        return "120";
    }
    return "60";
}

[[nodiscard]] inline bool parse_frame_rate_cap(std::string_view text, FrameRateCap& out) {
    const std::string lower = ascii_lower(perf_detail::trim_ascii_ws(text));
    if (lower == "30" || lower == "30hz") {
        out = FrameRateCap::Fps30;
        return true;
    }
    if (lower == "60" || lower == "60hz") {
        out = FrameRateCap::Fps60;
        return true;
    }
    if (lower == "90" || lower == "90hz") {
        out = FrameRateCap::Fps90;
        return true;
    }
    if (lower == "120" || lower == "120hz") {
        out = FrameRateCap::Fps120;
        return true;
    }
    if (lower == "0" || lower == "uncapped" || lower == "unlimited") {
        out = FrameRateCap::Uncapped;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr FrameRateCap frame_rate_cap_from_hz(std::int32_t hz) noexcept {
    if (hz <= 0) {
        return FrameRateCap::Uncapped;
    }
    if (hz <= 30) {
        return FrameRateCap::Fps30;
    }
    if (hz <= 60) {
        return FrameRateCap::Fps60;
    }
    if (hz <= 90) {
        return FrameRateCap::Fps90;
    }
    return FrameRateCap::Fps120;
}

// Vulkan swapchain / SDL vsync present mode preference.
// `Fifo` maps to `VK_PRESENT_MODE_FIFO_KHR` (guaranteed in Vulkan 1.0, vsync),
// `Mailbox` -> `VK_PRESENT_MODE_MAILBOX_KHR`,
// `Immediate` -> `VK_PRESENT_MODE_IMMEDIATE_KHR`,
// `FifoRelaxed` -> `VK_PRESENT_MODE_FIFO_RELAXED_KHR`.
enum class PresentModePreference : std::uint8_t {
    Fifo = 0,
    Mailbox = 1,
    Immediate = 2,
    FifoRelaxed = 3,
};

[[nodiscard]] constexpr std::string_view present_mode_name(
    PresentModePreference mode) noexcept {
    switch (mode) {
    case PresentModePreference::Fifo:
        return "fifo";
    case PresentModePreference::Mailbox:
        return "mailbox";
    case PresentModePreference::Immediate:
        return "immediate";
    case PresentModePreference::FifoRelaxed:
        return "fifo_relaxed";
    }
    return "fifo";
}

[[nodiscard]] inline bool parse_present_mode(std::string_view text,
                                             PresentModePreference& out) {
    const std::string lower = ascii_lower(perf_detail::trim_ascii_ws(text));
    if (lower == "fifo") {
        out = PresentModePreference::Fifo;
        return true;
    }
    if (lower == "mailbox") {
        out = PresentModePreference::Mailbox;
        return true;
    }
    if (lower == "immediate") {
        out = PresentModePreference::Immediate;
        return true;
    }
    if (lower == "fifo_relaxed") {
        out = PresentModePreference::FifoRelaxed;
        return true;
    }
    return false;
}

[[nodiscard]] constexpr PresentModePreference select_present_mode(
    FrameRateCap effective_cap,
    PresentModePreference requested,
    bool supports_mailbox,
    bool supports_immediate,
    bool supports_fifo_relaxed) noexcept {
    switch (requested) {
    case PresentModePreference::Immediate:
        if (effective_cap == FrameRateCap::Uncapped && supports_immediate) {
            return PresentModePreference::Immediate;
        }
        if (supports_mailbox) {
            return PresentModePreference::Mailbox;
        }
        if (effective_cap != FrameRateCap::Uncapped && supports_fifo_relaxed) {
            return PresentModePreference::FifoRelaxed;
        }
        return PresentModePreference::Fifo;
    case PresentModePreference::Mailbox:
        if (supports_mailbox) {
            return PresentModePreference::Mailbox;
        }
        return PresentModePreference::Fifo;
    case PresentModePreference::FifoRelaxed:
        if (supports_fifo_relaxed) {
            return PresentModePreference::FifoRelaxed;
        }
        return PresentModePreference::Fifo;
    case PresentModePreference::Fifo:
    default:
        return PresentModePreference::Fifo;
    }
}

// Translates a resolved present mode + cap into an `SDL_SetRenderVSync` parameter
// (`1` = vsync, `0` = disabled, `-1` = adaptive).
[[nodiscard]] constexpr int sdl_vsync_for_present_mode(
    PresentModePreference mode, FrameRateCap effective_cap) noexcept {
    switch (mode) {
    case PresentModePreference::Immediate:
        return 0;
    case PresentModePreference::Mailbox:
        return (effective_cap == FrameRateCap::Uncapped) ? 0 : 1;
    case PresentModePreference::FifoRelaxed:
        return -1;
    case PresentModePreference::Fifo:
    default:
        return 1;
    }
}

// UI aspect mode stored for P11 launcher/runtime integration (unused by the runtime yet).
enum class UiAspectMode : std::uint8_t {
    Ratio4x3 = 0,
    Ratio16x9 = 1,
    Stretch = 2,
};

[[nodiscard]] constexpr std::string_view ui_aspect_mode_key(UiAspectMode mode) noexcept {
    switch (mode) {
    case UiAspectMode::Ratio4x3:
        return "4:3";
    case UiAspectMode::Ratio16x9:
        return "16:9";
    case UiAspectMode::Stretch:
        return "stretch";
    }
    return "4:3";
}

[[nodiscard]] inline bool parse_ui_aspect_mode(std::string_view text, UiAspectMode& out) {
    const std::string lower = ascii_lower(perf_detail::trim_ascii_ws(text));
    if (lower == "4:3") {
        out = UiAspectMode::Ratio4x3;
        return true;
    }
    if (lower == "16:9") {
        out = UiAspectMode::Ratio16x9;
        return true;
    }
    if (lower == "stretch") {
        out = UiAspectMode::Stretch;
        return true;
    }
    return false;
}

enum class QualityPreset : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
    Custom = 3,
};

[[nodiscard]] constexpr std::string_view quality_preset_key(QualityPreset preset) noexcept {
    switch (preset) {
    case QualityPreset::Low:
        return "low";
    case QualityPreset::Medium:
        return "medium";
    case QualityPreset::High:
        return "high";
    case QualityPreset::Custom:
        return "custom";
    }
    return "custom";
}

[[nodiscard]] inline bool parse_quality_preset(std::string_view text, QualityPreset& out) {
    const std::string lower = ascii_lower(perf_detail::trim_ascii_ws(text));
    if (lower == "low") {
        out = QualityPreset::Low;
        return true;
    }
    if (lower == "medium") {
        out = QualityPreset::Medium;
        return true;
    }
    if (lower == "high") {
        out = QualityPreset::High;
        return true;
    }
    if (lower == "custom") {
        out = QualityPreset::Custom;
        return true;
    }
    return false;
}

// Matches Android `PowerManager.THERMAL_STATUS_*` (`0..6`).
enum class ThermalStatus : std::uint8_t {
    None = 0,
    Light = 1,
    Moderate = 2,
    Severe = 3,
    Critical = 4,
    Emergency = 5,
    Shutdown = 6,
};

[[nodiscard]] constexpr ThermalStatus sanitize_thermal_status(std::int32_t raw) noexcept {
    if (raw <= 0) {
        return ThermalStatus::None;
    }
    if (raw >= static_cast<std::int32_t>(ThermalStatus::Shutdown)) {
        return ThermalStatus::Shutdown;
    }
    return static_cast<ThermalStatus>(raw);
}

[[nodiscard]] constexpr std::string_view thermal_status_name(ThermalStatus status) noexcept {
    switch (status) {
    case ThermalStatus::None:
        return "none";
    case ThermalStatus::Light:
        return "light";
    case ThermalStatus::Moderate:
        return "moderate";
    case ThermalStatus::Severe:
        return "severe";
    case ThermalStatus::Critical:
        return "critical";
    case ThermalStatus::Emergency:
        return "emergency";
    case ThermalStatus::Shutdown:
        return "shutdown";
    }
    return "none";
}

enum class ThermalTier : std::uint8_t {
    Nominal = 0,
    Elevated = 1,
    Throttled = 2,
    Critical = 3,
};

[[nodiscard]] constexpr std::string_view thermal_tier_name(ThermalTier tier) noexcept {
    switch (tier) {
    case ThermalTier::Nominal:
        return "nominal";
    case ThermalTier::Elevated:
        return "elevated";
    case ThermalTier::Throttled:
        return "throttled";
    case ThermalTier::Critical:
        return "critical";
    }
    return "nominal";
}

// Thermal measurement reported by the OS.
// Matches Android `PowerManager.getThermalHeadroom(forecastSeconds)`:
//   - `0.0` = cool baseline
//   - `1.0` = `THERMAL_STATUS_SEVERE` threshold reached
//   - `> 1.0` = beyond severe throttling
//   - `< 0.0` or `NaN` = headroom unavailable (`headroom_valid == false`)
struct ThermalSnapshot {
    ThermalStatus status = ThermalStatus::None;
    float headroom = 0.0f;
    bool headroom_valid = false;

    [[nodiscard]] static ThermalSnapshot make(std::int32_t raw_status,
                                              float raw_headroom) noexcept {
        ThermalSnapshot snap{};
        snap.status = sanitize_thermal_status(raw_status);
        if (std::isfinite(raw_headroom) && raw_headroom >= 0.0f) {
            snap.headroom = std::clamp(raw_headroom, 0.0f, 4.0f);
            snap.headroom_valid = true;
        }
        return snap;
    }

    [[nodiscard]] bool operator==(const ThermalSnapshot&) const noexcept = default;
};

struct PerformanceSettings {
    std::int32_t version = 1;
    QualityPreset quality_preset = QualityPreset::Custom;
    FrameRateCap frame_rate_cap = FrameRateCap::Fps60;
    PresentModePreference present_mode = PresentModePreference::Fifo;
    float render_scale = 1.0f;
    // Maximum texture dimension cap; 0 means no cap.
    std::uint32_t max_texture_size = 0U;
    // UI aspect and scale are stored for P11 and not used by the runtime yet.
    UiAspectMode ui_aspect = UiAspectMode::Ratio4x3;
    float ui_scale = 1.0f;
    std::string language = "en";
    bool thermal_throttling_enabled = true;
    bool dynamic_resolution_enabled = true;
    bool lod_bias_scaling_enabled = true;
    float min_resolution_scale = 0.65f;
    float max_lod_bias = 1.5f;
    // Launcher toggles stored here too, so `config/performance.json` is the one saved copy
    // (the launcher reads and writes it through JNI; `STATUS.md` §5 P8 review item 4). They
    // seed `InputProfile::gyro_aim_enabled` / `pinch_zoom_enabled` at launch.
    bool gyro_aim_enabled = false;
    bool pinch_zoom_enabled = true;

    [[nodiscard]] bool operator==(const PerformanceSettings&) const noexcept = default;
};

namespace perf_detail {

[[nodiscard]] inline bool is_valid_language_tag(std::string_view lang) noexcept {
    if (lang.empty() || lang.size() > 16U) {
        return false;
    }
    for (const char c : lang) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

}  // namespace perf_detail

inline void sanitize_performance_settings(PerformanceSettings& s) noexcept {
    const PerformanceSettings def{};
    const auto finite_or = [](float v, float fallback) noexcept {
        return std::isfinite(v) ? v : fallback;
    };
    s.version = 1;
    if (static_cast<std::uint8_t>(s.quality_preset) >
        static_cast<std::uint8_t>(QualityPreset::Custom)) {
        s.quality_preset = def.quality_preset;
    }
    s.frame_rate_cap = frame_rate_cap_from_hz(frame_rate_cap_hz(s.frame_rate_cap));
    if (static_cast<std::uint8_t>(s.present_mode) >
        static_cast<std::uint8_t>(PresentModePreference::FifoRelaxed)) {
        s.present_mode = def.present_mode;
    }
    s.render_scale = std::clamp(finite_or(s.render_scale, def.render_scale), 0.50f, 1.0f);
    if (s.max_texture_size != 0U) {
        const std::uint32_t clamped = std::clamp(s.max_texture_size, 256U, 4096U);
        std::uint32_t pow2 = 256U;
        while (pow2 <= clamped / 2U) {
            pow2 *= 2U;
        }
        s.max_texture_size = pow2;
    }
    if (static_cast<std::uint8_t>(s.ui_aspect) >
        static_cast<std::uint8_t>(UiAspectMode::Stretch)) {
        s.ui_aspect = def.ui_aspect;
    }
    s.ui_scale = std::clamp(finite_or(s.ui_scale, def.ui_scale), 0.75f, 2.0f);
    if (!perf_detail::is_valid_language_tag(s.language)) {
        s.language = def.language;
    }
    s.min_resolution_scale =
        std::clamp(finite_or(s.min_resolution_scale, def.min_resolution_scale), 0.50f, 1.0f);
    s.max_lod_bias = std::clamp(finite_or(s.max_lod_bias, def.max_lod_bias), 0.0f, 3.0f);
}

inline constexpr QualityPreset kFirstLaunchQualityPreset = QualityPreset::Medium;

inline void apply_quality_preset(PerformanceSettings& s, QualityPreset preset) noexcept {
    switch (preset) {
    case QualityPreset::Low:
        s.quality_preset = QualityPreset::Low;
        s.frame_rate_cap = FrameRateCap::Fps30;
        s.render_scale = 0.75f;
        s.max_texture_size = 512U;
        break;
    case QualityPreset::Medium:
        s.quality_preset = QualityPreset::Medium;
        s.frame_rate_cap = FrameRateCap::Fps30;
        s.render_scale = 0.85f;
        s.max_texture_size = 1024U;
        break;
    case QualityPreset::High:
        s.quality_preset = QualityPreset::High;
        s.frame_rate_cap = FrameRateCap::Fps60;
        s.render_scale = 1.0f;
        s.max_texture_size = 2048U;
        break;
    case QualityPreset::Custom:
    default:
        s.quality_preset = QualityPreset::Custom;
        break;
    }
}

namespace perf_detail {

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

inline bool parse_uint32_exact(std::string_view s, std::uint32_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    std::uint32_t val = 0;
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

inline std::string fmt_float(float v) {
    char buf[48];
    const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return (ec == std::errc{}) ? std::string(buf, ptr) : std::string("0");
}

// Parses a flat settings/choices JSON object and overlays its present keys onto `in_out`.
// Leaves `in_out` unchanged if the JSON is malformed or any key has an invalid value.
[[nodiscard]] inline bool overlay_performance_settings_json(std::string_view json,
                                                            PerformanceSettings& in_out,
                                                            std::string* error_out) {
    std::map<std::string, std::string> raw;
    if (!wse::core::wse_parse_json_object(json, raw)) {
        if (error_out != nullptr) {
            *error_out = "malformed performance settings JSON";
        }
        return false;
    }
    PerformanceSettings s = in_out;
    if (auto it = raw.find("version"); it != raw.end()) {
        std::int32_t v = 0;
        if (!parse_int_exact(it->second, v) || v != 1) {
            if (error_out != nullptr) {
                *error_out = "unsupported performance settings version";
            }
            return false;
        }
    }
    // Apply quality_preset before individual overrides so explicit keys win in the same payload.
    if (auto it = raw.find("quality_preset"); it != raw.end()) {
        QualityPreset preset{};
        if (!parse_quality_preset(it->second, preset)) {
            if (error_out != nullptr) {
                *error_out = "invalid quality_preset in performance settings";
            }
            return false;
        }
        apply_quality_preset(s, preset);
    }
    if (auto it = raw.find("frame_rate_cap"); it != raw.end()) {
        if (!parse_frame_rate_cap(it->second, s.frame_rate_cap)) {
            if (error_out != nullptr) {
                *error_out = "invalid frame_rate_cap in performance settings";
            }
            return false;
        }
    }
    if (auto it = raw.find("present_mode"); it != raw.end()) {
        if (!parse_present_mode(it->second, s.present_mode)) {
            if (error_out != nullptr) {
                *error_out = "invalid present_mode in performance settings";
            }
            return false;
        }
    }
    if (auto it = raw.find("max_texture_size"); it != raw.end()) {
        if (!parse_uint32_exact(it->second, s.max_texture_size)) {
            if (error_out != nullptr) {
                *error_out = "invalid max_texture_size in performance settings";
            }
            return false;
        }
    }
    if (auto it = raw.find("ui_aspect"); it != raw.end()) {
        if (!parse_ui_aspect_mode(it->second, s.ui_aspect)) {
            if (error_out != nullptr) {
                *error_out = "invalid ui_aspect in performance settings";
            }
            return false;
        }
    }
    if (auto it = raw.find("language"); it != raw.end()) {
        if (!is_valid_language_tag(it->second)) {
            if (error_out != nullptr) {
                *error_out = "invalid language in performance settings";
            }
            return false;
        }
        s.language = it->second;
    }
    for (const auto& [key, field] : {
             std::pair{"thermal_throttling_enabled", &s.thermal_throttling_enabled},
             std::pair{"dynamic_resolution_enabled", &s.dynamic_resolution_enabled},
             std::pair{"lod_bias_scaling_enabled", &s.lod_bias_scaling_enabled},
             std::pair{"gyro_aim_enabled", &s.gyro_aim_enabled},
             std::pair{"pinch_zoom_enabled", &s.pinch_zoom_enabled},
         }) {
        if (auto it = raw.find(key); it != raw.end() && !parse_bool_exact(it->second, *field)) {
            if (error_out != nullptr) {
                *error_out = std::string("invalid ") + key;
            }
            return false;
        }
    }
    for (const auto& [key, field] : {
             std::pair{"render_scale", &s.render_scale},
             std::pair{"ui_scale", &s.ui_scale},
             std::pair{"min_resolution_scale", &s.min_resolution_scale},
             std::pair{"max_lod_bias", &s.max_lod_bias},
         }) {
        if (auto it = raw.find(key); it != raw.end() && !parse_float_exact(it->second, *field)) {
            if (error_out != nullptr) {
                *error_out = std::string("invalid ") + key;
            }
            return false;
        }
    }
    sanitize_performance_settings(s);
    in_out = std::move(s);
    return true;
}

}  // namespace perf_detail

[[nodiscard]] inline std::string performance_settings_vfs_path() {
    return "config/performance.json";
}

[[nodiscard]] inline std::string serialize_performance_settings_json(
    const PerformanceSettings& raw) {
    PerformanceSettings s = raw;
    sanitize_performance_settings(s);
    std::map<std::string, std::string> entries;
    entries["version"] = "1";
    entries["quality_preset"] = std::string(quality_preset_key(s.quality_preset));
    entries["frame_rate_cap"] = std::string(frame_rate_cap_key(s.frame_rate_cap));
    entries["present_mode"] = std::string(present_mode_name(s.present_mode));
    entries["render_scale"] = perf_detail::fmt_float(s.render_scale);
    entries["max_texture_size"] = std::to_string(s.max_texture_size);
    entries["ui_aspect"] = std::string(ui_aspect_mode_key(s.ui_aspect));
    entries["ui_scale"] = perf_detail::fmt_float(s.ui_scale);
    entries["language"] = s.language;
    entries["thermal_throttling_enabled"] = s.thermal_throttling_enabled ? "true" : "false";
    entries["dynamic_resolution_enabled"] = s.dynamic_resolution_enabled ? "true" : "false";
    entries["lod_bias_scaling_enabled"] = s.lod_bias_scaling_enabled ? "true" : "false";
    entries["min_resolution_scale"] = perf_detail::fmt_float(s.min_resolution_scale);
    entries["max_lod_bias"] = perf_detail::fmt_float(s.max_lod_bias);
    entries["gyro_aim_enabled"] = s.gyro_aim_enabled ? "true" : "false";
    entries["pinch_zoom_enabled"] = s.pinch_zoom_enabled ? "true" : "false";
    return wse::core::wse_serialize_json_object(entries, true);
}

[[nodiscard]] inline bool deserialize_performance_settings_json(
    std::string_view json,
    PerformanceSettings& out,
    std::string* error_out = nullptr) {
    PerformanceSettings s{};
    if (!perf_detail::overlay_performance_settings_json(json, s, error_out)) {
        return false;
    }
    out = std::move(s);
    return true;
}

[[nodiscard]] inline bool save_performance_settings(Vfs& vfs,
                                                    const PerformanceSettings& settings,
                                                    std::string* error_out = nullptr) {
    const std::string path = performance_settings_vfs_path();
    const std::string json = serialize_performance_settings_json(settings);
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(json.data()), json.size());
    // `config/` does not exist on a fresh install; a failure surfaces at the write.
    (void)make_parent_dirs(vfs, path);
    const std::string tmp = path + ".partial";
    if (!vfs.write_file(tmp, bytes)) {
        if (error_out != nullptr) {
            *error_out = "failed writing performance settings";
        }
        return false;
    }
    if (!vfs.rename_file(tmp, path)) {
        (void)vfs.remove_file(tmp);
        if (!vfs.write_file(path, bytes)) {
            if (error_out != nullptr) {
                *error_out = "failed committing performance settings";
            }
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool load_performance_settings(Vfs& vfs,
                                                    PerformanceSettings& out,
                                                    std::string* error_out = nullptr) {
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(performance_settings_vfs_path(), raw)) {
        if (error_out != nullptr) {
            *error_out = "performance settings not found";
        }
        return false;
    }
    const std::string_view sv(reinterpret_cast<const char*>(raw.data()), raw.size());
    return deserialize_performance_settings_json(sv, out, error_out);
}

// Loads `config/performance.json`, falling back to the defaults. `present_out` (optional)
// says whether the file existed; a corrupt or unsupported file yields the defaults and
// `false`, a missing one the defaults and `true`.
[[nodiscard]] inline bool load_performance_settings_or_default(Vfs& vfs,
                                                               PerformanceSettings& out,
                                                               bool* present_out = nullptr,
                                                               std::string* error_out = nullptr) {
    const bool present = vfs.is_file(performance_settings_vfs_path());
    if (present_out != nullptr) {
        *present_out = present;
    }
    PerformanceSettings loaded{};
    if (present && !load_performance_settings(vfs, loaded, error_out)) {
        out = PerformanceSettings{};
        return false;
    }
    out = loaded;
    return true;
}

// Fields the launcher edits; the rest of `config/performance.json` (resolution floor, LOD
// bias ceiling) is kept as stored.
struct LauncherPerformanceChoices {
    FrameRateCap frame_rate_cap = FrameRateCap::Fps60;
    PresentModePreference present_mode = PresentModePreference::Fifo;
    bool thermal_throttling_enabled = true;
    bool dynamic_resolution_enabled = true;
    bool lod_bias_scaling_enabled = true;
    bool gyro_aim_enabled = false;
    bool pinch_zoom_enabled = true;
};

inline void apply_launcher_choices(PerformanceSettings& s, const LauncherPerformanceChoices& c) noexcept {
    s.frame_rate_cap = c.frame_rate_cap;
    s.present_mode = c.present_mode;
    s.thermal_throttling_enabled = c.thermal_throttling_enabled;
    s.dynamic_resolution_enabled = c.dynamic_resolution_enabled;
    s.lod_bias_scaling_enabled = c.lod_bias_scaling_enabled;
    s.gyro_aim_enabled = c.gyro_aim_enabled;
    s.pinch_zoom_enabled = c.pinch_zoom_enabled;
    sanitize_performance_settings(s);
}

// Load (or default), apply the launcher's choices, save. A corrupt file is replaced.
[[nodiscard]] inline bool save_launcher_choices(Vfs& vfs,
                                                const LauncherPerformanceChoices& c,
                                                PerformanceSettings* out = nullptr,
                                                std::string* error_out = nullptr) {
    PerformanceSettings s{};
    (void)load_performance_settings_or_default(vfs, s);
    apply_launcher_choices(s, c);
    if (out != nullptr) {
        *out = s;
    }
    return save_performance_settings(vfs, s, error_out);
}

// Ensures `config/performance.json` exists and is valid; missing, unreadable, or corrupt
// files are replaced with defaults plus `kFirstLaunchQualityPreset`.
[[nodiscard]] inline bool ensure_first_launch_settings(Vfs& vfs,
                                                       PerformanceSettings& out,
                                                       bool* created = nullptr,
                                                       std::string* error_out = nullptr) {
    if (created != nullptr) {
        *created = false;
    }
    PerformanceSettings loaded{};
    if (load_performance_settings(vfs, loaded)) {
        out = std::move(loaded);
        return true;
    }
    PerformanceSettings defaults{};
    apply_quality_preset(defaults, kFirstLaunchQualityPreset);
    sanitize_performance_settings(defaults);
    if (!save_performance_settings(vfs, defaults, error_out)) {
        return false;
    }
    out = std::move(defaults);
    if (created != nullptr) {
        *created = true;
    }
    return true;
}

// Loads (or defaults) `config/performance.json`, overlays a partial JSON object of launcher
// choices (applying `quality_preset` first when present), sanitizes, and saves. Any invalid
// value fails without writing.
[[nodiscard]] inline bool save_launcher_settings_json(Vfs& vfs,
                                                      std::string_view choices_json,
                                                      PerformanceSettings* out = nullptr,
                                                      std::string* error_out = nullptr) {
    PerformanceSettings s{};
    (void)load_performance_settings_or_default(vfs, s);
    if (!perf_detail::overlay_performance_settings_json(choices_json, s, error_out)) {
        return false;
    }
    if (!save_performance_settings(vfs, s, error_out)) {
        return false;
    }
    if (out != nullptr) {
        *out = s;
    }
    return true;
}

struct PerformanceAdaptation {
    ThermalTier tier = ThermalTier::Nominal;
    ThermalStatus thermal_status = ThermalStatus::None;
    float thermal_headroom = 0.0f;
    bool thermal_headroom_valid = false;
    float resolution_scale = 1.0f;
    float lod_bias = 0.0f;
    std::int32_t requested_fps_cap = 60;
    std::int32_t effective_fps_cap = 60;
    PresentModePreference active_present_mode = PresentModePreference::Fifo;
};

struct ScaledExtent {
    std::uint32_t width = 1U;
    std::uint32_t height = 1U;
};

[[nodiscard]] inline ScaledExtent scale_render_extent(std::uint32_t width,
                                                      std::uint32_t height,
                                                      float resolution_scale,
                                                      std::uint32_t min_dim = 64U) noexcept {
    const std::uint32_t safe_w = std::max(1U, width);
    const std::uint32_t safe_h = std::max(1U, height);
    const float s = std::isfinite(resolution_scale) ? std::clamp(resolution_scale, 0.25f, 1.0f) : 1.0f;
    const auto scale_axis = [s, min_dim](std::uint32_t axis) noexcept -> std::uint32_t {
        if (axis <= min_dim) {
            return axis;
        }
        const auto scaled = static_cast<std::uint32_t>(std::lround(static_cast<double>(axis) * s));
        return std::clamp(scaled, min_dim, axis);
    };
    return ScaledExtent{scale_axis(safe_w), scale_axis(safe_h)};
}

// Translates OS thermal status & headroom readings into hysteretic performance budgets
// (resolution scale, LOD bias, effective FPS cap step-down, and Vulkan present mode).
class ThermalPerformanceController {
public:
    static constexpr float kElevatedEnterHeadroom = 0.65f;
    static constexpr float kElevatedExitHeadroom = 0.57f;
    static constexpr float kThrottledEnterHeadroom = 0.80f;
    static constexpr float kThrottledExitHeadroom = 0.72f;
    static constexpr float kCriticalEnterHeadroom = 0.95f;
    static constexpr float kCriticalExitHeadroom = 0.87f;
    // Headroom-less updates a valid reading is held across (`WseActivity` polls headroom
    // every 10 s; status listener callbacks in between carry none).
    static constexpr std::uint32_t kMaxHeldHeadroomUpdates = 16U;

    ThermalPerformanceController() { sanitize_performance_settings(settings_); }

    void set_settings(PerformanceSettings s) noexcept {
        sanitize_performance_settings(s);
        settings_ = s;
        if (!settings_.thermal_throttling_enabled) {
            tier_ = ThermalTier::Nominal;
        } else {
            recompute_tier();
        }
    }

    [[nodiscard]] const PerformanceSettings& settings() const noexcept { return settings_; }

    void set_present_capabilities(bool supports_mailbox,
                                  bool supports_immediate,
                                  bool supports_fifo_relaxed) noexcept {
        supports_mailbox_ = supports_mailbox;
        supports_immediate_ = supports_immediate;
        supports_fifo_relaxed_ = supports_fifo_relaxed;
    }

    // A reading without headroom (status-only listener callbacks, a rate-limited or
    // failed `getThermalHeadroom`) carries no new headroom information, so the last
    // valid headroom keeps steering the tier for up to `kMaxHeldHeadroomUpdates`
    // updates instead of being read as "cool".
    void update_thermal(const ThermalSnapshot& snap) noexcept {
        snapshot_ = snap;
        if (snap.headroom_valid) {
            held_headroom_ = snap.headroom;
            held_headroom_valid_ = true;
            held_headroom_updates_ = 0U;
        } else if (held_headroom_valid_ && ++held_headroom_updates_ > kMaxHeldHeadroomUpdates) {
            held_headroom_valid_ = false;
        }
        recompute_tier();
    }

    void update_thermal(std::int32_t raw_status, float raw_headroom) noexcept {
        update_thermal(ThermalSnapshot::make(raw_status, raw_headroom));
    }

    [[nodiscard]] const ThermalSnapshot& thermal() const noexcept { return snapshot_; }
    [[nodiscard]] ThermalTier tier() const noexcept { return tier_; }
    // Headroom the tier logic currently uses (the latest valid reading, possibly held).
    [[nodiscard]] bool effective_headroom(float& out) const noexcept {
        out = held_headroom_;
        return held_headroom_valid_;
    }

    [[nodiscard]] PerformanceAdaptation evaluate() const noexcept {
        PerformanceAdaptation out{};
        out.tier = tier_;
        out.thermal_status = snapshot_.status;
        out.thermal_headroom = snapshot_.headroom;
        out.thermal_headroom_valid = snapshot_.headroom_valid;

        const std::int32_t requested = frame_rate_cap_hz(settings_.frame_rate_cap);
        out.requested_fps_cap = requested;
        out.effective_fps_cap = requested;

        if (settings_.thermal_throttling_enabled && tier_ != ThermalTier::Nominal) {
            std::int32_t tier_fps_limit = requested;
            float tier_scale = 1.0f;
            float tier_lod = 0.0f;
            switch (tier_) {
            case ThermalTier::Elevated:
                tier_fps_limit = 90;
                tier_scale = 0.90f;
                tier_lod = 0.35f;
                break;
            case ThermalTier::Throttled:
                tier_fps_limit = 60;
                tier_scale = 0.80f;
                tier_lod = 0.75f;
                break;
            case ThermalTier::Critical:
                tier_fps_limit = 30;
                tier_scale = settings_.min_resolution_scale;
                tier_lod = settings_.max_lod_bias;
                break;
            case ThermalTier::Nominal:
            default:
                break;
            }
            out.effective_fps_cap =
                (requested == 0) ? tier_fps_limit : std::min(requested, tier_fps_limit);
            if (settings_.dynamic_resolution_enabled) {
                out.resolution_scale = std::clamp(tier_scale, settings_.min_resolution_scale, 1.0f);
            }
            if (settings_.lod_bias_scaling_enabled) {
                out.lod_bias = std::clamp(tier_lod, 0.0f, settings_.max_lod_bias);
            }
        }

        const FrameRateCap eff_cap = frame_rate_cap_from_hz(out.effective_fps_cap);
        out.active_present_mode =
            select_present_mode(eff_cap, settings_.present_mode, supports_mailbox_,
                                supports_immediate_, supports_fifo_relaxed_);
        return out;
    }

private:
    [[nodiscard]] static constexpr ThermalTier tier_for_status(ThermalStatus s) noexcept {
        if (s >= ThermalStatus::Severe) {
            return ThermalTier::Critical;
        }
        if (s >= ThermalStatus::Moderate) {
            return ThermalTier::Throttled;
        }
        return (s >= ThermalStatus::Light) ? ThermalTier::Elevated : ThermalTier::Nominal;
    }

    [[nodiscard]] static constexpr ThermalTier tier_for_headroom(float h) noexcept {
        if (h >= kCriticalEnterHeadroom) {
            return ThermalTier::Critical;
        }
        if (h >= kThrottledEnterHeadroom) {
            return ThermalTier::Throttled;
        }
        return (h >= kElevatedEnterHeadroom) ? ThermalTier::Elevated : ThermalTier::Nominal;
    }

    [[nodiscard]] static constexpr float exit_headroom(ThermalTier t) noexcept {
        switch (t) {
        case ThermalTier::Critical:
            return kCriticalExitHeadroom;
        case ThermalTier::Throttled:
            return kThrottledExitHeadroom;
        case ThermalTier::Elevated:
            return kElevatedExitHeadroom;
        case ThermalTier::Nominal:
        default:
            return 0.0f;
        }
    }

    // Escalation is immediate (OS status or headroom past an enter threshold). Recovery
    // needs headroom at or below the current tier's exit bound and moves down one tier
    // per update, so a cooled device ramps 30 -> 60 -> 90 Hz instead of jumping straight
    // back to full load and reheating.
    void recompute_tier() noexcept {
        if (!settings_.thermal_throttling_enabled) {
            tier_ = ThermalTier::Nominal;
            return;
        }
        const bool hv = held_headroom_valid_;
        const float h = held_headroom_;
        ThermalTier target = tier_for_status(snapshot_.status);
        if (hv) {
            target = std::max(target, tier_for_headroom(h));
        }
        if (target >= tier_) {
            tier_ = target;
            return;
        }
        if (!hv || h <= exit_headroom(tier_)) {
            tier_ = static_cast<ThermalTier>(static_cast<std::uint8_t>(tier_) - 1U);
        }
    }

    PerformanceSettings settings_{};
    ThermalSnapshot snapshot_{};
    ThermalTier tier_ = ThermalTier::Nominal;
    float held_headroom_ = 0.0f;
    bool held_headroom_valid_ = false;
    std::uint32_t held_headroom_updates_ = 0U;
    bool supports_mailbox_ = false;
    bool supports_immediate_ = false;
    bool supports_fifo_relaxed_ = false;
};

// Clock-driven drift-free frame pacer for 60 / 90 / 120 Hz (or uncapped) loops.
// Tracks exact integer + fractional nanoseconds across `kTicksPerSecond = 100000`
// (`1 tick = 10000 ns`) so tests using `ManualClock` are completely deterministic.
class FramePacer {
public:
    static constexpr std::int64_t kNsPerSecond = 1000000000LL;
    static constexpr std::int64_t kNsPerTick = kNsPerSecond / kTicksPerSecond;  // 10,000 ns

    explicit FramePacer(Clock& clock) noexcept : clock_(&clock) {}

    void set_target_fps(std::int32_t fps_cap) noexcept {
        const std::int32_t clamped = (fps_cap <= 0) ? 0 : std::clamp(fps_cap, 1, 240);
        if (target_fps_ != clamped) {
            target_fps_ = clamped;
            reset();
        }
    }

    [[nodiscard]] std::int32_t target_fps() const noexcept { return target_fps_; }

    void reset() noexcept {
        has_deadline_ = false;
        next_deadline_ns_ = 0;
        frac_acc_ = 0;
    }

    // Call once when work for the current frame begins; advances the target deadline
    // by one exact frame interval.
    void begin_frame() noexcept {
        if (target_fps_ <= 0) {
            reset();
            return;
        }
        const std::int64_t now_ns = clock_->ticks() * kNsPerTick;
        if (!has_deadline_) {
            next_deadline_ns_ = now_ns + next_interval_ns();
            has_deadline_ = true;
            return;
        }
        const std::int64_t step_ns = next_interval_ns();
        next_deadline_ns_ += step_ns;
        // Drop accumulated catch-up if the app paused or hit a multi-frame hitch.
        if (now_ns - next_deadline_ns_ > step_ns * 2) {
            next_deadline_ns_ = now_ns + step_ns;
            frac_acc_ = 0;
        }
    }

    // Returns remaining nanoseconds to wait before starting the next frame (`0` if late
    // or uncapped).
    [[nodiscard]] std::int64_t remaining_wait_ns() noexcept {
        if (target_fps_ <= 0 || !has_deadline_) {
            return 0;
        }
        const std::int64_t now_ns = clock_->ticks() * kNsPerTick;
        return std::max<std::int64_t>(0, next_deadline_ns_ - now_ns);
    }

    // Equivalent whole `Clock` ticks (rounded up so sleeping reaches the deadline).
    [[nodiscard]] std::int64_t remaining_wait_ticks() noexcept {
        const std::int64_t ns = remaining_wait_ns();
        return (ns <= 0) ? 0 : ((ns + kNsPerTick - 1) / kNsPerTick);
    }

private:
    [[nodiscard]] std::int64_t next_interval_ns() noexcept {
        const std::int64_t base = kNsPerSecond / target_fps_;
        const std::int64_t rem = kNsPerSecond % target_fps_;
        frac_acc_ += rem;
        std::int64_t extra = 0;
        if (frac_acc_ >= target_fps_) {
            frac_acc_ -= target_fps_;
            extra = 1;
        }
        return base + extra;
    }

    Clock* clock_;
    std::int32_t target_fps_ = 60;
    bool has_deadline_ = false;
    std::int64_t next_deadline_ns_ = 0;
    std::int64_t frac_acc_ = 0;
};

// Rolling frame-time statistics over the last `kWindow` frames, for an on-screen FPS
// readout that shows what a cap actually achieves on a device (vsync quantization,
// thermal clocks, pacing overshoot). Gaps longer than `kMaxFrameGapNs` (a background
// pause or a breakpoint) restart the measurement instead of counting as one frame.
class FrameTimeStats {
public:
    static constexpr std::size_t kWindow = 120U;
    static constexpr std::int64_t kMaxFrameGapNs = 250000000LL;  // 250 ms

    void reset() noexcept {
        has_origin_ = false;
        count_ = 0U;
        next_ = 0U;
    }

    // Call once per presented frame with a monotonic timestamp.
    void record_frame(std::int64_t now_ns) noexcept {
        if (!has_origin_) {
            has_origin_ = true;
            last_ns_ = now_ns;
            return;
        }
        const std::int64_t dt = now_ns - last_ns_;
        last_ns_ = now_ns;
        if (dt <= 0) {
            return;
        }
        if (dt > kMaxFrameGapNs) {
            count_ = 0U;
            next_ = 0U;
            return;
        }
        samples_[next_] = dt;
        next_ = (next_ + 1U) % kWindow;
        count_ = std::min(count_ + 1U, kWindow);
    }

    [[nodiscard]] std::size_t sample_count() const noexcept { return count_; }

    [[nodiscard]] double average_frame_ms() const noexcept {
        if (count_ == 0U) {
            return 0.0;
        }
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            sum += samples_[i];
        }
        return static_cast<double>(sum) / static_cast<double>(count_) / 1.0e6;
    }

    [[nodiscard]] double average_fps() const noexcept {
        const double ms = average_frame_ms();
        return (ms > 0.0) ? 1000.0 / ms : 0.0;
    }

    [[nodiscard]] double worst_frame_ms() const noexcept {
        std::int64_t worst = 0;
        for (std::size_t i = 0; i < count_; ++i) {
            worst = std::max(worst, samples_[i]);
        }
        return static_cast<double>(worst) / 1.0e6;
    }

    // "1% low": the frame rate over the slowest 1% of frames in the window (at least
    // the single slowest one), so one hitch in a 120-frame window shows up.
    [[nodiscard]] double low_1pct_fps() const noexcept {
        if (count_ == 0U) {
            return 0.0;
        }
        std::array<std::int64_t, kWindow> sorted{};
        std::copy_n(samples_.begin(), count_, sorted.begin());
        const std::size_t k = std::max<std::size_t>(1U, count_ / 100U);
        std::partial_sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k),
                          sorted.begin() + static_cast<std::ptrdiff_t>(count_),
                          std::greater<>());
        std::int64_t sum = 0;
        for (std::size_t i = 0; i < k; ++i) {
            sum += sorted[i];
        }
        return (sum > 0) ? 1.0e9 * static_cast<double>(k) / static_cast<double>(sum) : 0.0;
    }

private:
    std::array<std::int64_t, kWindow> samples_{};
    std::size_t count_ = 0U;
    std::size_t next_ = 0U;
    std::int64_t last_ns_ = 0;
    bool has_origin_ = false;
};

// What the frame loop applies at launch: the whole `config/performance.json`.
struct RuntimeLaunchPerfConfig {
    PerformanceSettings settings{};
};

// Thread-safe process bridge connecting Android JNI callbacks (`WseActivity` ->
// `saf_vfs_jni.cpp`) with the SDL main loop (`sdl_selftest_main.cpp`).
class RuntimePerformanceHub {
public:
    static RuntimePerformanceHub& global() noexcept {
        static RuntimePerformanceHub instance;
        return instance;
    }

    void set_launch_config(const RuntimeLaunchPerfConfig& cfg) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        launch_config_ = cfg;
        ++config_revision_;
    }

    [[nodiscard]] bool poll_launch_config(std::uint64_t& last_seen_revision,
                                          RuntimeLaunchPerfConfig& out) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (config_revision_ == last_seen_revision) {
            return false;
        }
        last_seen_revision = config_revision_;
        out = launch_config_;
        return true;
    }

    void update_thermal_state(std::int32_t raw_status, float raw_headroom) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        thermal_ = ThermalSnapshot::make(raw_status, raw_headroom);
        ++thermal_revision_;
    }

    [[nodiscard]] ThermalSnapshot thermal_snapshot(
        std::uint64_t* out_revision = nullptr) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (out_revision != nullptr) {
            *out_revision = thermal_revision_;
        }
        return thermal_;
    }

private:
    mutable std::mutex mutex_{};
    RuntimeLaunchPerfConfig launch_config_{};
    std::uint64_t config_revision_ = 0U;
    ThermalSnapshot thermal_{};
    std::uint64_t thermal_revision_ = 0U;
};

}  // namespace wse::platform
