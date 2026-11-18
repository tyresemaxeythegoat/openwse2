// Milestone 6 world extension types and shared runtime helpers (204 WSE opcodes).
// Grounded in WSE2 v1.1.5.5 binary helpers (mbnetBuffer, WSEAntiCheat, mbAgent,
// mbTerrainGenerator, mbMenu, mbPresentationContainer, WSELuaManager).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/core/math.h"
#include "wse/game/world_subsystem_types.h"

namespace wse::game {

// -----------------------------------------------------------------------------
// Bit-stream network buffer mirroring mbnetBuffer.
// -----------------------------------------------------------------------------
struct WseNetBuffer {
    std::vector<std::uint8_t> bytes{};
    std::int32_t size = 0;   // total written bits
    std::int32_t cursor = 0; // current bit cursor

    void clear() noexcept {
        bytes.clear();
        size = 0;
        cursor = 0;
    }

    void ensure_bits(std::int32_t total_bits) {
        if (total_bits <= 0) {
            return;
        }
        const auto needed = static_cast<std::size_t>((total_bits + 7) / 8);
        if (bytes.size() < needed) {
            bytes.resize(needed, 0U);
        }
    }

    void write_bits(std::uint32_t value, int bits) {
        if (bits <= 0 || bits > 32) {
            return;
        }
        ensure_bits(cursor + bits);
        for (int i = 0; i < bits; ++i) {
            const std::int32_t pos = cursor + i;
            const auto byte_idx = static_cast<std::size_t>(pos >> 3);
            const auto bit_mask = static_cast<std::uint8_t>(1U << (pos & 7));
            if (((value >> i) & 1U) != 0U) {
                bytes[byte_idx] |= bit_mask;
            } else {
                bytes[byte_idx] &= static_cast<std::uint8_t>(~bit_mask);
            }
        }
        cursor += bits;
        size = std::max(size, cursor);
    }

    [[nodiscard]] std::uint32_t read_bits(int bits) noexcept {
        if (bits <= 0 || bits > 32 || cursor + bits > size) {
            return 0U;
        }
        std::uint32_t out = 0U;
        for (int i = 0; i < bits; ++i) {
            const std::int32_t pos = cursor + i;
            const auto byte_idx = static_cast<std::size_t>(pos >> 3);
            if (byte_idx < bytes.size() &&
                ((bytes[byte_idx] >> (pos & 7)) & 1U) != 0U) {
                out |= (std::uint32_t{1} << i);
            }
        }
        cursor += bits;
        return out;
    }

    // mbnetBuffer packed ints.
    void put_uint16(std::uint16_t value, int bits) {
        write_bits(static_cast<std::uint32_t>(value), bits);
    }

    [[nodiscard]] std::uint16_t take_uint16(int bits) noexcept {
        return static_cast<std::uint16_t>(read_bits(bits));
    }

    void put_int32(std::int32_t value, int bits, std::int32_t min_val = 0) {
        if (bits <= 0 || bits > 32) {
            return;
        }
        if (bits == 32 || min_val == std::numeric_limits<std::int32_t>::min()) {
            write_bits(static_cast<std::uint32_t>(value), bits);
        } else {
            const std::uint32_t shifted =
                static_cast<std::uint32_t>(value) -
                static_cast<std::uint32_t>(min_val);
            write_bits(shifted, bits);
        }
    }

    [[nodiscard]] std::int32_t take_int32(int bits,
                                          std::int32_t min_val = 0) noexcept {
        if (bits <= 0 || bits > 32 || cursor + bits > size) {
            return 0;
        }
        const std::uint32_t raw = read_bits(bits);
        if (bits == 32 || min_val == std::numeric_limits<std::int32_t>::min()) {
            return static_cast<std::int32_t>(raw);
        }
        return static_cast<std::int32_t>(raw +
                                          static_cast<std::uint32_t>(min_val));
    }

    // mbnetBuffer packed strings: max length < 300 chars (16-bit len + bytes).
    void put_string(std::string_view utf8, int max_len = 300) {
        const std::size_t cap =
            max_len > 1 ? static_cast<std::size_t>(max_len - 1) : 0U;
        const std::size_t n = std::min(utf8.size(), cap);
        put_uint16(static_cast<std::uint16_t>(n), 16);
        for (std::size_t i = 0; i < n; ++i) {
            write_bits(static_cast<std::uint8_t>(utf8[i]), 8);
        }
    }

    [[nodiscard]] std::string take_string(int max_len = 300) {
        std::string out;
        if (cursor + 16 > size) {
            return out;
        }
        const std::uint16_t n = take_uint16(16);
        const std::int32_t needed_bits = static_cast<std::int32_t>(n) * 8;
        if (static_cast<int>(n) >= max_len || cursor + needed_bits > size) {
            return out;
        }
        out.resize(n);
        for (std::uint16_t i = 0; i < n; ++i) {
            out[i] = static_cast<char>(read_bits(8) & 0xFFU);
        }
        return out;
    }

    [[nodiscard]] static std::uint32_t quantize(float v,
                                                float min_v,
                                                float step) noexcept {
        if (!std::isfinite(v) || step <= 0.0f) {
            return 0U;
        }
        const double q = static_cast<double>(v - min_v) / static_cast<double>(step);
        if (q <= 0.0) {
            return 0U;
        }
        if (q >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            return std::numeric_limits<std::uint32_t>::max();
        }
        return static_cast<std::uint32_t>(static_cast<std::int64_t>(q));
    }

    // Exact match for rglTransform::setRotation (0x1402b6840) and inverse.
    static void quat_to_transform(float w,
                                  float x,
                                  float y,
                                  float z,
                                  wse::core::WseTransform& t) noexcept {
        const float n2 = w * w + x * x + y * y + z * z;
        if (n2 <= 1.0e-12f) {
            t.s = {1.0f, 0.0f, 0.0f, 0.0f};
            t.f = {0.0f, 1.0f, 0.0f, 0.0f};
            t.u = {0.0f, 0.0f, 1.0f, 0.0f};
            return;
        }
        const float inv = 1.0f / std::sqrt(n2);
        w *= inv;
        x *= inv;
        y *= inv;
        z *= inv;
        t.s = {1.0f - 2.0f * (y * y + z * z),
               2.0f * (x * y + w * z),
               2.0f * (x * z - w * y),
               0.0f};
        t.f = {2.0f * (x * y - w * z),
               1.0f - 2.0f * (x * x + z * z),
               2.0f * (y * z + w * x),
               0.0f};
        t.u = {2.0f * (x * z + w * y),
               2.0f * (y * z - w * x),
               1.0f - 2.0f * (x * x + y * y),
               0.0f};
    }

    static void transform_to_quat(const wse::core::WseTransform& t,
                                  float& w,
                                  float& x,
                                  float& y,
                                  float& z) noexcept {
        const float trace = t.s.x + t.f.y + t.u.z;
        if (trace > 0.0f) {
            const float s = 2.0f * std::sqrt(trace + 1.0f);
            w = 0.25f * s;
            x = (t.f.z - t.u.y) / s;
            y = (t.u.x - t.s.z) / s;
            z = (t.s.y - t.f.x) / s;
        } else if (t.s.x > t.f.y && t.s.x > t.u.z) {
            const float s = 2.0f * std::sqrt(1.0f + t.s.x - t.f.y - t.u.z);
            w = (t.f.z - t.u.y) / s;
            x = 0.25f * s;
            y = (t.f.x + t.s.y) / s;
            z = (t.u.x + t.s.z) / s;
        } else if (t.f.y > t.u.z) {
            const float s = 2.0f * std::sqrt(1.0f + t.f.y - t.s.x - t.u.z);
            w = (t.u.x - t.s.z) / s;
            x = (t.f.x + t.s.y) / s;
            y = 0.25f * s;
            z = (t.u.y + t.f.z) / s;
        } else {
            const float s = 2.0f * std::sqrt(1.0f + t.u.z - t.s.x - t.f.y);
            w = (t.s.y - t.f.x) / s;
            x = (t.u.x + t.s.z) / s;
            y = (t.u.y + t.f.z) / s;
            z = 0.25f * s;
        }
        const float norm = std::sqrt(w * w + x * x + y * y + z * z);
        if (norm > 1.0e-12f) {
            const float inv = 1.0f / norm;
            w *= inv;
            x *= inv;
            y *= inv;
            z *= inv;
        } else {
            w = 1.0f;
            x = y = z = 0.0f;
        }
    }

    // 0x1401b56a0 / 0x1401b6520: vector4 pack/unpack (10 or 17 bits/axis).
    void put_vector4(const wse::core::WseVec4& v, bool compressed) {
        const int bits = compressed ? 10 : 17;
        const float min_v = compressed ? -10.0f : -50.0f;
        const float step = compressed ? 0.02f : 0.01f;
        write_bits(quantize(v.x, min_v, step), bits);
        write_bits(quantize(v.y, min_v, step), bits);
        write_bits(quantize(v.z, min_v, step), bits);
    }

    [[nodiscard]] wse::core::WseVec4 take_vector4(bool compressed) noexcept {
        const int bits = compressed ? 10 : 17;
        const float min_v = compressed ? -10.0f : -50.0f;
        const float step = compressed ? 0.02f : 0.01f;
        if (cursor + bits * 3 > size) {
            return {};
        }
        const float rx = static_cast<float>(read_bits(bits)) * step + min_v;
        const float ry = static_cast<float>(read_bits(bits)) * step + min_v;
        const float rz = static_cast<float>(read_bits(bits)) * step + min_v;
        return {rx, ry, rz, 0.0f};
    }

    // 0x1401b58a0 / 0x1401b67b0: transform pack/unpack (origin + 4x12-bit quat).
    void put_transform(const wse::core::WseTransform& t, bool compressed) {
        const int bits = compressed ? 10 : 17;
        const float min_v = compressed ? -10.0f : -50.0f;
        const float step = compressed ? 0.02f : 0.01f;
        write_bits(quantize(t.o.x, min_v, step), bits);
        write_bits(quantize(t.o.y, min_v, step), bits);
        write_bits(quantize(t.o.z, min_v, step), bits);
        float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
        transform_to_quat(t, w, x, y, z);
        write_bits(quantize(w, -1.01f, 0.0005f), 12);
        write_bits(quantize(x, -1.01f, 0.0005f), 12);
        write_bits(quantize(y, -1.01f, 0.0005f), 12);
        write_bits(quantize(z, -1.01f, 0.0005f), 12);
    }

    void take_transform(wse::core::WseTransform& out, bool compressed) noexcept {
        const int bits = compressed ? 10 : 17;
        const float min_v = compressed ? -10.0f : -50.0f;
        const float step = compressed ? 0.02f : 0.01f;
        if (cursor + (bits + 16) * 3 > size) {
            return;
        }
        out.o.x = static_cast<float>(read_bits(bits)) * step + min_v;
        out.o.y = static_cast<float>(read_bits(bits)) * step + min_v;
        out.o.z = static_cast<float>(read_bits(bits)) * step + min_v;
        out.o.w = 0.0f;
        const float qw = static_cast<float>(read_bits(12)) * 0.0005f - 1.01f;
        const float qx = static_cast<float>(read_bits(12)) * 0.0005f - 1.01f;
        const float qy = static_cast<float>(read_bits(12)) * 0.0005f - 1.01f;
        const float qz = static_cast<float>(read_bits(12)) * 0.0005f - 1.01f;
        quat_to_transform(qw, qx, qy, qz, out);
    }
};

struct WseSentCompositeMessage {
    std::int32_t target_player_no = -1; // -1 = sent to server
    std::int32_t message_id = 0;
    WseNetBuffer payload{};
};

// -----------------------------------------------------------------------------
// AntiCheat state mirroring WSEAntiCheat.
// -----------------------------------------------------------------------------
struct WseAntiCheatReportEvent {
    std::int32_t player_no = -1;
    std::int32_t category = 0;
    std::int64_t param1 = 0;
    std::int64_t param2 = 0;
    bool notify_server = false;
};

struct WseAntiCheatPlayerRecord {
    // Integer stats: {0,1,3,5,7,8,9,10,12,13,14,15,17,18,19,21}.
    // Float stats (scaled by fp_scale on read): {2,4,6,11,16,20,22,23,24,25}.
    std::array<std::int64_t, 26> int_stats{};
    std::array<float, 26> float_stats{};
    std::array<double, 32> last_report_sec{};

    WseAntiCheatPlayerRecord() noexcept {
        last_report_sec.fill(-1.0e9);
    }
};

struct WseAntiCheatOptionEntry {
    enum class Kind : std::uint8_t { unmapped = 0, boolean, integer, scalar };
    Kind kind = Kind::unmapped;
    bool b_val = false;
    std::int32_t i_val = 0;
    float f_val = 0.0f;
    std::int32_t clamp_min = std::numeric_limits<std::int32_t>::min();
    std::int32_t clamp_max = std::numeric_limits<std::int32_t>::max();
    bool clamp_nonneg_float = false;
};

struct WseAntiCheatState {
    bool initialized = true;
    double current_time_sec = 0.0;
    std::array<WseAntiCheatOptionEntry, 87> options{};
    std::unordered_map<std::int32_t, WseAntiCheatPlayerRecord> players{};
    std::vector<WseAntiCheatReportEvent> reports{};

    WseAntiCheatState() noexcept {
        auto set_i = [&](int id, std::int32_t def, std::int32_t lo, std::int32_t hi) {
            options[static_cast<std::size_t>(id)] = {
                WseAntiCheatOptionEntry::Kind::integer, false, def, 0.0f, lo, hi, false};
        };
        auto set_b = [&](int id, bool def) {
            options[static_cast<std::size_t>(id)] = {
                WseAntiCheatOptionEntry::Kind::boolean, def, 0, 0.0f, 0, 1, false};
        };
        auto set_f = [&](int id, float def, bool nonneg = false) {
            options[static_cast<std::size_t>(id)] = {
                WseAntiCheatOptionEntry::Kind::scalar, false, 0, def, 0, 0, nonneg};
        };
        constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
        constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
        set_i(0, 2, 0, 2);
        set_b(1, true);
        set_b(2, true);
        set_i(3, 0, 0, kMax);
        set_f(4, 30.0f, true);
        set_b(10, true);
        set_b(11, false);
        set_i(12, 3, 1, kMax);
        set_b(20, true);
        set_f(21, 2400.0f);
        set_i(22, 150, kMin, kMax);
        set_i(23, 6, 1, kMax);
        set_f(24, 60.0f);
        set_b(30, true);
        set_f(31, 0.1f);
        set_i(32, 3, 1, kMax);
        set_i(33, 8, 1, 64);
        set_b(40, true);
        set_i(41, 30, 1, 64);
        set_f(42, 0.06f);
        set_b(50, false);
        set_i(51, 16, kMin, kMax);
        set_i(52, 10, kMin, kMax);
        set_f(53, 5.0f);
        set_b(60, true);
        set_i(61, 40, 1, 64);
        set_f(62, 0.9f);
        set_i(63, 120, kMin, kMax);
        set_i(64, 120, kMin, kMax);
        set_i(65, 8, 1, kMax);
        set_f(66, 140.0f);
        set_i(67, 6, 1, kMax);
        set_b(70, true);
        set_i(71, 40, 1, 64);
        set_f(72, 0.9f);
        set_i(73, 120, kMin, kMax);
        set_i(74, 120, kMin, kMax);
        set_i(75, 8, 1, kMax);
        set_f(76, 0.6f);
        set_i(77, 30, 1, kMax);
        set_b(80, true);
        set_f(81, 10.0f);
        set_f(82, 80.0f);
        set_i(83, 25, 1, 64);
        set_f(84, 20.0f);
        set_f(85, 0.8f);
        set_f(86, 1.5f);
    }

    [[nodiscard]] std::int64_t get_option(std::int32_t id,
                                          std::int32_t fp_mul) const noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= options.size()) {
            return 0;
        }
        const auto& opt = options[static_cast<std::size_t>(id)];
        switch (opt.kind) {
        case WseAntiCheatOptionEntry::Kind::boolean:
            return opt.b_val ? 1 : 0;
        case WseAntiCheatOptionEntry::Kind::integer:
            return opt.i_val;
        case WseAntiCheatOptionEntry::Kind::scalar:
            return static_cast<std::int64_t>(opt.f_val * static_cast<float>(fp_mul));
        default:
            return 0;
        }
    }

    void set_option(std::int32_t id,
                    std::int64_t wide_val,
                    std::int32_t narrow_val,
                    std::int32_t fp_mul) noexcept {
        if (id < 0 || static_cast<std::size_t>(id) >= options.size()) {
            return;
        }
        auto& opt = options[static_cast<std::size_t>(id)];
        switch (opt.kind) {
        case WseAntiCheatOptionEntry::Kind::boolean:
            opt.b_val = (wide_val != 0);
            break;
        case WseAntiCheatOptionEntry::Kind::integer:
            opt.i_val = std::clamp(narrow_val, opt.clamp_min, opt.clamp_max);
            break;
        case WseAntiCheatOptionEntry::Kind::scalar: {
            float v = (fp_mul != 0)
                          ? (static_cast<float>(wide_val) / static_cast<float>(fp_mul))
                          : 0.0f;
            if (opt.clamp_nonneg_float && v < 0.0f) {
                v = 0.0f;
            }
            opt.f_val = v;
            break;
        }
        default:
            break;
        }
    }

    [[nodiscard]] std::int64_t get_stat_scaled(std::int32_t player_no,
                                               std::int32_t stat_id,
                                               std::int32_t scale) const noexcept {
        if (player_no < 0 || stat_id < 0 || stat_id > 25) {
            return 0;
        }
        const auto it = players.find(player_no);
        if (it == players.end()) {
            return 0;
        }
        const auto& rec = it->second;
        switch (stat_id) {
        case 2:
        case 4:
        case 6:
        case 11:
        case 16:
        case 20:
        case 22:
        case 23:
        case 24:
        case 25:
            return static_cast<std::int64_t>(
                rec.float_stats[static_cast<std::size_t>(stat_id)] *
                static_cast<float>(scale));
        default:
            return rec.int_stats[static_cast<std::size_t>(stat_id)];
        }
    }

    bool report(std::int32_t player_no,
                std::int32_t category,
                std::int64_t param1,
                std::int64_t param2,
                bool notify_server,
                std::int32_t player_privilege) {
        if (!initialized || player_no < 0) {
            return false;
        }
        if (player_privilege > 2 && options[2].b_val) {
            return false;
        }
        const std::size_t cat =
            static_cast<std::size_t>(std::clamp(category, 0, 31));
        const double cooldown =
            static_cast<double>(std::max(0.0f, options[4].f_val));
        auto& rec = players[player_no];
        if (rec.last_report_sec[cat] + cooldown > current_time_sec) {
            return false;
        }
        rec.last_report_sec[cat] = current_time_sec;
        rec.int_stats[0] += 1;
        reports.push_back(
            {player_no, static_cast<std::int32_t>(cat), param1, param2, notify_server});
        return true;
    }
};

// -----------------------------------------------------------------------------
// Player, profile, server list, agent, scene, UI, skeleton, Lua extensions.
// -----------------------------------------------------------------------------
struct WseM6ProfileExt {
    std::string name{};
    std::int32_t gender_skin = 0;
};

struct WseM6PlayerExt {
    std::int32_t wse2_version = 0;
    std::int32_t party_no = -1;
    std::string ip_address{};
    bool controlling_stopped = false;
    std::int32_t temp_banned_minutes = 0;
};

struct WseM6ServerListEntry {
    std::string ip{};
    std::int32_t port = 7240;
    std::vector<std::int32_t> module_settings{};
};

struct WseM6ServerExt {
    std::string admin_password{};
    std::string private_password{};
    std::string active_server_ip{};
    std::int32_t active_server_port = 7240;
    std::vector<WseM6ServerListEntry> server_list{};
    std::vector<std::string> anonymous_players{};
    std::unordered_map<std::uint32_t, std::string> anonymous_player_names{};
    bool horse_friendly_fire = false;
    bool show_crosshair = true;
    bool ghost_advanced_enabled = false;
    bool ghost_free_roam = true;
    bool ghost_lock_to_team = false;
    bool ghost_lock_to_agent = false;
    bool ghost_disable_free_cam = false;
    bool ghost_show_hud = false;
    bool ghost_only_players = false;
    std::int32_t forced_lod = 0;
    std::string stored_connect_address{};
    std::string stored_connect_password{};
    bool switching_module = false;
    bool switching_to_campaign = false;
    bool connect_to_server = false;
    bool break_warband_compatibility = true;
};

// Default 12 rider rotation intervals.
inline constexpr std::array<std::array<float, 2>, 12> kDefaultRiderRotationAngles{{
    {0.0f, 0.0f},
    {-0.5f, 3.141593f},
    {-1.3f, 2.8f},
    {-1.5f, 2.5f},
    {-3.0f, 1.0f},
    {-2.0f, 2.0f},
    {-1.5f, 2.4f},
    {-2.0f, 2.0f},
    {-2.0f, 2.0f},
    {-0.65f, 0.75f},
    {-1.3f, 3.0f},
    {-2.0f, 2.0f},
}};

struct WseM6AgentExt {
    std::int32_t horse_item_modifier = 0;
    std::array<std::int32_t, 10> item_slot_modifiers{};
    std::array<std::uint32_t, 10> item_slot_flags{};
    float scale = 1.0f;
    std::int32_t forced_lod = 0;
    wse::core::WseVec4 ai_move_target_position{};
    wse::core::WseVec4 ai_move_target_velocity{0.0f, 1.0f, 0.0f, 0.0f};
    wse::core::WseVec4 ai_formation_position{};
    float ai_behavior_update_time = 0.0f;
    wse::core::WseVec4 acceleration{};
    float ghost_timer = 0.0f;
    float deform_vertex_keys_time = 0.0f;
    std::array<float, 29> body_mesh_deform_times{};
    std::array<float, 29> body_mesh_deform_progress{
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    bool body_meta_mesh_visible = true;
    std::unordered_map<std::int32_t, bool> body_submesh_visibility{};
    std::unordered_map<std::int32_t, std::int32_t> personal_animations{};
    float stun_time = 0.0f;
    std::int32_t body_deform_start = 0;
    std::int32_t body_deform_end = 0;
    float body_deform_duration = 0.0f;
    bool body_deform_loop = false;
    float body_deform_progress = 0.0f;
    std::int32_t footstep_sound_no = -1;
    std::array<std::int32_t, 5> footstep_sound_nos{-1, -1, -1, -1, -1};
    std::int32_t voice_sound_no = -1;
    std::array<std::int32_t, 8> voice_sound_nos{-1, -1, -1, -1, -1, -1, -1, -1};
    float horse_rotation_velocity = 0.0f;
    float vertical_speed = 0.0f;
    std::int32_t position_in_group = 0;
    std::int32_t current_ai_mesh_face_group = -1;
    float time_speed_multiplier = 1.0f;
    bool kick_requested = false;
    float dropped_items_prune_time = 0.0f;
    float missile_items_prune_time = 0.0f;
    bool left_hand_weapon_collision = false;
    bool ai_can_weapon_switch = true;
    bool ai_can_fight = true;
    bool ai_can_aim = true;
    float fade_out_duration = 0.0f;
    float fade_out_alpha = 0.0f;
    float fade_in_duration = 0.0f;
    bool enable_tilt = true;
    std::array<std::array<float, 2>, 12> rider_rotation_angles =
        kDefaultRiderRotationAngles;
    bool can_multihit = false;
    std::int32_t multihit_limit = 0;
    std::int32_t crush_through_mode = 0;
    std::uint32_t disabled_triggers = 0U; // bits 0..3 per 0x14002b8b0/0x14002b8f0
    bool can_piercing = false;
    std::int32_t piercing_limit = 0;
};

struct WseM6SiteExt {
    std::int32_t flags = 0;
    float water_level = 0.0f;
    wse::core::WseVec2 min_bound{-100.0f, -100.0f};
    wse::core::WseVec2 max_bound{100.0f, 100.0f};
    std::string outer_terrain{};
    std::array<std::uint32_t, 6> terrain_keys{};
    std::string terrain_code{};

    // Mirrors mbTerrainGenerator::getTerrainCode (0x1402315b0): keys[5..0].
    void sync_terrain_code() {
        char buf[64]{};
        std::snprintf(buf, sizeof(buf), "0x%.8x%.8x%.8x%.8x%.8x%.8x",
                     terrain_keys[5], terrain_keys[4], terrain_keys[3],
                     terrain_keys[2], terrain_keys[1], terrain_keys[0]);
        terrain_code = buf;
    }

    // Mirrors mbTerrainGenerator bitfield setters (from 0x140231770).
    void set_seed(int idx, std::int32_t val) {
        if (idx < 0 || idx > 2) {
            return;
        }
        const auto u = static_cast<std::uint32_t>(std::min(val, 0x7fffffff)) & 0x7fffffffU;
        terrain_keys[static_cast<std::size_t>(idx)] =
            (terrain_keys[static_cast<std::size_t>(idx)] & 0x80000000U) | u;
        sync_terrain_code();
    }

    void set_deep_water(std::int32_t v) {
        const std::uint32_t bit = static_cast<std::uint32_t>(std::min(v, 1) & 1) << 31;
        terrain_keys[1] = (terrain_keys[1] & 0x7fffffffU) | bit;
        sync_terrain_code();
    }

    void set_place_river(std::int32_t v) {
        const std::uint32_t bit = static_cast<std::uint32_t>(std::min(v, 1) & 1) << 31;
        terrain_keys[3] = (terrain_keys[3] & 0x7fffffffU) | bit;
        sync_terrain_code();
    }

    void set_disable_grass(std::int32_t v) {
        const std::uint32_t f = static_cast<std::uint32_t>(std::min(v, 2) & 3) << 2;
        terrain_keys[5] = (terrain_keys[5] & 0xfffffff3U) | f;
        sync_terrain_code();
    }

    void set_size(float sx, float sy) {
        const auto xbits = static_cast<std::uint32_t>(
            std::min(static_cast<std::int32_t>(sx), 0x3ff) & 0x3ff);
        const auto ybits = static_cast<std::uint32_t>(
            std::min(static_cast<std::int32_t>(sy), 0x3ff) & 0x3ff);
        terrain_keys[3] = (terrain_keys[3] & 0xfff00000U) | xbits | (ybits << 10);
        sync_terrain_code();
    }

    void set_valley_size(float v) {
        const int iv = std::clamp(static_cast<int>(std::lround(v * 100.0f)), 0, 99);
        terrain_keys[4] = (terrain_keys[4] & 0xffffff80U) | static_cast<std::uint32_t>(iv);
        sync_terrain_code();
    }

    void set_hill_height(float v) {
        const int iv = std::clamp(static_cast<int>(v), 0, 127);
        terrain_keys[4] = (terrain_keys[4] & 0xffffc07fU) | (static_cast<std::uint32_t>(iv) << 7);
        sync_terrain_code();
    }

    void set_ruggedness(std::int32_t v) {
        const auto f = static_cast<std::uint32_t>(std::min(v, 127) & 0x7f) << 14;
        terrain_keys[4] = (terrain_keys[4] & 0xffe03fffU) | f;
        sync_terrain_code();
    }

    void set_vegetation(float v) {
        const int iv = std::clamp(static_cast<int>(std::lround(v * 100.0f)), 0, 99);
        terrain_keys[4] = (terrain_keys[4] & 0xf01fffffU) | (static_cast<std::uint32_t>(iv) << 21);
        sync_terrain_code();
    }

    void set_region_type(std::int32_t v) {
        const auto f = static_cast<std::uint32_t>(std::min(v, 16) & 0xf) << 28;
        terrain_keys[4] = (terrain_keys[4] & 0x0fffffffU) | f;
        sync_terrain_code();
    }

    void set_region_detail(std::int32_t v) {
        const auto f = static_cast<std::uint32_t>(std::min(v, 3) & 3);
        terrain_keys[5] = (terrain_keys[5] & 0xfffffffcU) | f;
        sync_terrain_code();
    }

    void set_region_variant(std::int32_t v) {
        const int iv = std::clamp(v, 0, 127);
        terrain_keys[5] = (terrain_keys[5] & 0x80ffffffU) | (static_cast<std::uint32_t>(iv) << 24);
        sync_terrain_code();
    }
};

struct WseM6MissileExt {
    bool remove_on_hit = false;
    bool removed = false;
    std::vector<wse::core::WseTransform> path_points{};
};

struct WseM6MissionObjectExt {
    std::int32_t forced_lod = 0;
    bool removed = false;
    float sound_progress = 0.0f;
    bool has_light = false;
};

struct WseM6SceneExt {
    float ally_collision_threshold = 0.0f;
    float ally_collision_min = 0.45f;
    float ally_collision_max = 0.60f;
    float prop_collision_threshold = 0.0f;
    std::array<std::array<float, 2>, 4> prop_collision_thresholds{{
        {0.40f, 0.80f},
        {0.40f, 0.80f},
        {0.40f, 0.80f},
        {0.40f, 0.75f},
    }};
    std::int32_t spectated_agent_no = -1;
    std::int32_t height_fog_height = 0;
    float height_fog_density = 0.1f;
    float height_fog_strength = 0.0f;
    bool height_fog_enabled = false;
    float water_wave_amplitude = 0.0f;
    float water_wave_scale = 1.0f;
    float water_wave_speed = 0.0f;
    float flora_wind_amplitude = 0.0f;
    float flora_wind_wavelength = 1.0f;
    float flora_wind_speed = 0.0f;
    bool sky_cube_rebake_requested = false;
    float weather_direction = -1000.0f;
    std::int32_t trigger_forced_lod = 0;
    std::vector<std::int32_t> removed_particle_systems{};
    std::unordered_map<std::int32_t, wse::core::WseVec4> ai_mesh_group_translations{};
};

struct WseM6DynamicMenuItem {
    std::string text{};
    std::int32_t condition_script_no = -1;
    std::int32_t consequence_script_no = -1;
};

struct WseM6DynamicMenu {
    std::string id{};
    std::string text{};
    std::string mesh_name{};
    std::int32_t flags = 0;
    std::int32_t enter_script_no = -1;
    std::vector<WseM6DynamicMenuItem> items{};
};

struct WseM6OverlayExt {
    std::int32_t button_type = 0;
    wse::core::WseVec2 scroll_pos{};
    bool enabled = true;
};

struct WseM6UrlRequest {
    std::string url{};
    std::string user_agent{};
    std::string post_data{};
    std::int32_t success_script_no = -1;
    std::int32_t failure_script_no = -1;
    bool skip_parsing = false;
    std::int32_t timeout_ms = -1;
    bool is_post = false;
    bool processed = false;
};

struct WseM6UiExt {
    bool order_flag_active = false;
    bool conversation_auto_update_choice = false;
    bool cursor_ray_valid = false;
    wse::core::WseVec4 cursor_ray_origin{0.0f, 0.0f, 100.0f, 0.0f};
    wse::core::WseVec4 cursor_ray_dir{0.0f, 0.0f, -1.0f, 0.0f};
    std::vector<std::pair<std::string, std::int32_t>> bink_videos{};
    std::vector<std::string> shell_opened_urls{};
    std::vector<std::pair<std::int32_t, std::string>> screenshots{};
    std::vector<std::string> url_advanced_requests{};
    std::vector<WseM6UrlRequest> url_advanced_queue{};
    std::vector<std::string> updated_materials{};
    bool reload_item_kinds_requested = false;
    bool reload_troops_requested = false;
    bool reload_parties_requested = false;
    std::string item_kinds_file{};
    std::string troops_file{};
    std::string parties_file{};
    bool conversation_presentation_started = false;
    // Bumped by every `start_presentation` (900), including one that names the presentation
    // already open, so `game::UiSession` can tell a self-restart (the Native refresh idiom
    // `presentation_set_duration 0` + `start_presentation <self>`) from no request at all
    // (port bookkeeping, not a retail field).
    std::uint32_t presentation_start_serial = 0U;
    // Bumped each time `consume_campaign_queued_window` opens menu window `0x12`, so the UI
    // rebuilds the menu even when a menu jumps to a menu (port bookkeeping).
    std::uint32_t menu_open_serial = 0U;
    std::vector<std::int32_t> active_presentations{};
    // Mission (conversation window `0xc`) presentations requested by
    // `start_conversation_presentation` (4907) and not yet started:
    // `mbConversationWindow::startPresentation` (`0x140088a00`) pushes onto `+0x2298`, and
    // `mbConversationWindow::updatePresentations` (`0x140088a20`) starts them next frame.
    std::vector<std::int32_t> pending_conversation_presentations{};
    // `presentation_set_duration` (902) naming a mission presentation other than the one whose
    // trigger is running: (presentation, centiseconds), applied by `game::UiSession` (the retail
    // mission branch writes the container found by `getPresentationContainer` `0x140089060`).
    std::vector<std::pair<std::int32_t, std::int32_t>> mission_duration_requests{};
    std::unordered_map<std::int32_t, WseM6OverlayExt> overlay_ext{};
    std::unordered_map<std::int32_t, WseM6DynamicMenu> menus{};
    std::vector<std::int32_t> generated_menu_ids{};
    std::int32_t base_num_menus = -1;
    std::int32_t active_window_id = -1;
    bool console_open = false;
    std::array<bool, 256> prev_key_down{};
    std::array<bool, 95> prev_game_key_down{};
    std::array<std::array<std::array<std::int32_t, 3>, 2>, 95> game_key_mappings{};
};

struct WseM6SkinExt {
    static constexpr std::uint32_t kDefaultBloodColor = 0xFF800000U;
    // Per-skin overrides written by skin_set_blood_color (5200).
    std::unordered_map<std::int32_t, std::uint32_t> blood_colors{};

    [[nodiscard]] std::uint32_t blood_color(std::int32_t skin_no) const {
        const auto it = blood_colors.find(skin_no);
        return it != blood_colors.end() ? it->second : kDefaultBloodColor;
    }

    std::unordered_map<std::string, std::array<std::int32_t, 64>> bone_body_sections{};
};

// -----------------------------------------------------------------------------
// Portable Lua 5.1 stack/env bridge mirroring WSELuaManager (5100..5114).
// -----------------------------------------------------------------------------
struct WseLuaValue {
    enum class Type : std::int32_t {
        none = -1,
        nil = 0,
        boolean = 1,
        lightuserdata = 2,
        number = 3,
        string = 4,
        table = 5,
        function = 6,
        userdata = 7,
        thread = 8,
    };

    Type type = Type::nil;
    std::int64_t int_val = 0;
    bool bool_val = false;
    std::string str_val{};
    bool is_pos = false;
    wse::core::WseTransform pos_val = wse::core::WseTransform::identity();

    [[nodiscard]] static WseLuaValue make_nil() noexcept {
        return {};
    }
    [[nodiscard]] static WseLuaValue make_bool(bool b) noexcept {
        WseLuaValue v{};
        v.type = Type::boolean;
        v.bool_val = b;
        return v;
    }
    [[nodiscard]] static WseLuaValue make_int(std::int64_t n) noexcept {
        WseLuaValue v{};
        v.type = Type::number;
        v.int_val = n;
        return v;
    }
    [[nodiscard]] static WseLuaValue make_str(std::string s) {
        WseLuaValue v{};
        v.type = Type::string;
        v.str_val = std::move(s);
        return v;
    }
    [[nodiscard]] static WseLuaValue make_pos(const wse::core::WseTransform& p) noexcept {
        WseLuaValue v{};
        v.type = Type::table; // WSELuaManager::lPushPos creates a Lua table
        v.is_pos = true;
        v.pos_val = p;
        return v;
    }
};

struct WseLuaState {
    using CallbackFn = std::function<int(WseLuaState&, std::int32_t)>;

    std::vector<WseLuaValue> stack{};
    std::unordered_map<std::string, CallbackFn> functions{};
    std::unordered_map<std::int32_t, CallbackFn> callbacks{};
    std::vector<std::string> errors{};
    std::int32_t lua_context = 0;

    [[nodiscard]] std::int32_t get_top() const noexcept {
        return static_cast<std::int32_t>(stack.size());
    }

    // Resolves Lua 1-based positive or negative stack index; returns -1 if invalid.
    [[nodiscard]] std::int32_t resolve_index(std::int32_t idx) const noexcept {
        const auto top = get_top();
        if (idx > 0) {
            return idx <= top ? (idx - 1) : -1;
        }
        if (idx < 0) {
            const std::int32_t pos = top + idx;
            return (pos >= 0 && pos < top) ? pos : -1;
        }
        return -1;
    }

    // Safety deviation: the stack is capped at kMaxLuaStackSize (Lua 5.1
    // LUAI_MAXCSTACK) and the error log at kMaxLuaErrors, so script operands
    // cannot drive unbounded allocations.
    static constexpr std::size_t kMaxLuaStackSize = 8000;
    static constexpr std::size_t kMaxLuaErrors = 256;

    void add_error(std::string message) {
        if (errors.size() >= kMaxLuaErrors) {
            errors.erase(errors.begin());
        }
        errors.push_back(std::move(message));
    }

    [[nodiscard]] bool has_room() const noexcept {
        return stack.size() < kMaxLuaStackSize;
    }

    // Returns false (stack untouched) when idx exceeds kMaxLuaStackSize.
    bool set_top(std::int32_t idx) {
        if (idx >= 0) {
            if (static_cast<std::size_t>(idx) > kMaxLuaStackSize) {
                return false;
            }
            stack.resize(static_cast<std::size_t>(idx), WseLuaValue::make_nil());
        } else {
            const std::int32_t new_top = get_top() + idx + 1;
            if (new_top >= 0) {
                stack.resize(static_cast<std::size_t>(new_top), WseLuaValue::make_nil());
            } else {
                stack.clear();
            }
        }
        return true;
    }

    void pop(std::int32_t n) {
        if (n <= 0) {
            return;
        }
        if (n >= get_top()) {
            stack.clear();
            return;
        }
        stack.resize(stack.size() - static_cast<std::size_t>(n));
    }

    void insert(std::int32_t idx) {
        const std::int32_t pos = resolve_index(idx);
        if (pos < 0 || stack.empty()) {
            return;
        }
        WseLuaValue top_val = std::move(stack.back());
        stack.pop_back();
        stack.insert(stack.begin() + pos, std::move(top_val));
    }

    void remove(std::int32_t idx) {
        const std::int32_t pos = resolve_index(idx);
        if (pos < 0) {
            return;
        }
        stack.erase(stack.begin() + pos);
    }

    [[nodiscard]] std::int32_t value_type(std::int32_t idx) const noexcept {
        const std::int32_t pos = resolve_index(idx);
        if (pos < 0) {
            return static_cast<std::int32_t>(WseLuaValue::Type::none);
        }
        return static_cast<std::int32_t>(stack[static_cast<std::size_t>(pos)].type);
    }

    [[nodiscard]] bool to_boolean(std::int32_t idx) const noexcept {
        const std::int32_t pos = resolve_index(idx);
        if (pos < 0) {
            return false;
        }
        const auto& v = stack[static_cast<std::size_t>(pos)];
        if (v.type == WseLuaValue::Type::nil) {
            return false;
        }
        if (v.type == WseLuaValue::Type::boolean) {
            return v.bool_val;
        }
        return true;
    }

    [[nodiscard]] std::int64_t to_integer(std::int32_t idx) const noexcept {
        const std::int32_t pos = resolve_index(idx);
        if (pos < 0) {
            return 0;
        }
        const auto& v = stack[static_cast<std::size_t>(pos)];
        return v.type == WseLuaValue::Type::number ? v.int_val : 0;
    }

    [[nodiscard]] const std::string* to_string(std::int32_t idx) const noexcept {
        const std::int32_t pos = resolve_index(idx);
        if (pos < 0) {
            return nullptr;
        }
        const auto& v = stack[static_cast<std::size_t>(pos)];
        if (v.type == WseLuaValue::Type::string) {
            return &v.str_val;
        }
        return nullptr;
    }

    [[nodiscard]] const wse::core::WseTransform* to_pos(
        std::int32_t idx) const noexcept {
        const std::int32_t pos = resolve_index(idx);
        if (pos < 0) {
            return nullptr;
        }
        const auto& v = stack[static_cast<std::size_t>(pos)];
        return v.is_pos ? &v.pos_val : nullptr;
    }

    void push_value(WseLuaValue value) {
        if (!has_room()) {
            add_error("stack overflow");
            return;
        }
        stack.push_back(std::move(value));
    }
    void push_int(std::int64_t n) {
        push_value(WseLuaValue::make_int(n));
    }
    void push_str(std::string s) {
        push_value(WseLuaValue::make_str(std::move(s)));
    }
    void push_pos(const wse::core::WseTransform& t) {
        push_value(WseLuaValue::make_pos(t));
    }
    void push_bool(bool b) {
        push_value(WseLuaValue::make_bool(b));
    }
    void push_nil() {
        push_value(WseLuaValue::make_nil());
    }
};

struct WseM6WorldExt {
    WseNetBuffer active_net_buffer{};
    bool has_trigger_net_buffer = false;
    std::unordered_map<std::int32_t, WseNetBuffer> net_buffers{};
    std::int32_t last_received_message_type = -1;
    std::int32_t last_received_sender_player = -1;
    std::vector<WseSentCompositeMessage> sent_composite_messages{};
    WseAntiCheatState anticheat{};
    WseM6ServerExt server{};
    WseM6SceneExt scene{};
    WseM6UiExt ui{};
    WseM6SkinExt skins{};
    WseLuaState lua{};
    std::unordered_map<std::int32_t, WseM6ProfileExt> profiles{};
    std::unordered_map<std::int32_t, WseM6PlayerExt> players{};
    std::unordered_map<std::int32_t, WseM6AgentExt> agents{};
    std::unordered_map<std::int32_t, WseM6SiteExt> sites{};
    std::unordered_map<std::int32_t, WseM6MissileExt> missiles{};
    // Missile that fired the running ti_on_missile_hit (-52) trigger, or -1.
    std::int32_t trigger_missile_no = -1;
    std::unordered_map<std::int32_t, WseM6MissionObjectExt> mission_objects{};
    std::vector<std::pair<std::int32_t, std::string>> chat_messages{};

    [[nodiscard]] WseM6PlayerExt& player(std::int32_t id) {
        return players[id];
    }
    [[nodiscard]] WseM6AgentExt& agent(std::int32_t id) {
        return agents[id];
    }
    [[nodiscard]] WseM6SiteExt& site(std::int32_t id) {
        return sites[id];
    }
    [[nodiscard]] WseM6MissileExt& missile(std::int32_t id) {
        return missiles[id];
    }
    [[nodiscard]] WseM6MissionObjectExt& mission_object(std::int32_t id) {
        return mission_objects[id];
    }
    [[nodiscard]] bool is_valid_missile(std::int32_t id) const noexcept {
        if (id < 0) {
            return false;
        }
        const auto it = missiles.find(id);
        return it != missiles.end() && !it->second.removed;
    }
};

} // namespace wse::game
