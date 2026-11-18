#pragma once

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

namespace wse::core {

// 2D vector matching rglVector2 (0x140319660 length, 0x1403196f0 getAngle).
struct WseVec2 {
    float x = 0.0f;
    float y = 0.0f;

    [[nodiscard]] float length() const noexcept {
        return std::sqrt(x * x + y * y);
    }

    // Mirrors rglVector2::getAngle (0x1403196f0): atan2f(-x, y).
    [[nodiscard]] float get_angle() const noexcept {
        return std::atan2(-x, y);
    }
};

// 64-byte position frame (rglTransform at 0x140664060, stride 0x40;
// Identity at 0x140622730): side s, forward f, up u, origin o.
struct WseVec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    [[nodiscard]] constexpr WseVec4 operator+(const WseVec4& rhs) const noexcept {
        return {x + rhs.x, y + rhs.y, z + rhs.z, w};
    }

    [[nodiscard]] constexpr WseVec4 operator-(const WseVec4& rhs) const noexcept {
        return {x - rhs.x, y - rhs.y, z - rhs.z, 0.0f};
    }

    [[nodiscard]] constexpr WseVec4 operator*(float scale) const noexcept {
        return {x * scale, y * scale, z * scale, w};
    }

    [[nodiscard]] float length() const noexcept {
        return std::sqrt(x * x + y * y + z * z);
    }

    [[nodiscard]] constexpr float dot(const WseVec4& rhs) const noexcept {
        return x * rhs.x + y * rhs.y + z * rhs.z;
    }

    [[nodiscard]] constexpr WseVec4 cross(const WseVec4& rhs) const noexcept {
        return {y * rhs.z - z * rhs.y,
                z * rhs.x - x * rhs.z,
                x * rhs.y - y * rhs.x,
                0.0f};
    }

    // Mirrors rglVector3::normalize (0x1403197d0): threshold 1e-10f (0x1405771d4),
    // fallback (0, 1, 0) while preserving w, returning pre-normalization length.
    [[nodiscard]] float normalize_with_length() noexcept {
        const float len = length();
        if (len <= 1.0e-10f) {
            x = 0.0f;
            y = 1.0f;
            z = 0.0f;
            return len;
        }
        const float inv = 1.0f / len;
        x *= inv;
        y *= inv;
        z *= inv;
        return len;
    }

    void normalize() noexcept {
        (void)normalize_with_length();
    }
};

struct WseTransform {
    WseVec4 s{1.0f, 0.0f, 0.0f, 0.0f};
    WseVec4 f{0.0f, 1.0f, 0.0f, 0.0f};
    WseVec4 u{0.0f, 0.0f, 1.0f, 0.0f};
    WseVec4 o{0.0f, 0.0f, 0.0f, 1.0f};

    [[nodiscard]] static constexpr WseTransform identity() noexcept {
        return {};
    }

    // 0x1402b5e50: rotates forward f and up u around local side axis s.
    void rotate_x(float angle) noexcept {
        const float c = std::cos(angle);
        const float s_val = std::sin(angle);
        const WseVec4 nf{f.x * c + u.x * s_val,
                         f.y * c + u.y * s_val,
                         f.z * c + u.z * s_val,
                         f.w};
        const WseVec4 nu{u.x * c - f.x * s_val,
                         u.y * c - f.y * s_val,
                         u.z * c - f.z * s_val,
                         u.w};
        f = nf;
        u = nu;
    }

    // 0x1402b5fb0: rotates side s and up u around local forward axis f.
    void rotate_y(float angle) noexcept {
        const float c = std::cos(angle);
        const float s_val = std::sin(angle);
        const WseVec4 ns{s.x * c - u.x * s_val,
                         s.y * c - u.y * s_val,
                         s.z * c - u.z * s_val,
                         u.w};
        const WseVec4 nu{s.x * s_val + u.x * c,
                         s.y * s_val + u.y * c,
                         s.z * s_val + u.z * c,
                         u.w};
        s = ns;
        u = nu;
    }

    // 0x1402b6110: rotates side s and forward f around local up axis u.
    void rotate_z(float angle) noexcept {
        const float c = std::cos(angle);
        const float s_val = std::sin(angle);
        const WseVec4 ns{s.x * c + f.x * s_val,
                         s.y * c + f.y * s_val,
                         s.z * c + f.z * s_val,
                         s.w};
        const WseVec4 nf{f.x * c - s.x * s_val,
                         f.y * c - s.y * s_val,
                         f.z * c - s.z * s_val,
                         f.w};
        s = ns;
        f = nf;
    }

    // 0x1402b6270: rotates basis vectors around global X axis.
    void rotate_global_x(float angle) noexcept {
        const float c = std::cos(angle);
        const float sn = std::sin(angle);
        const float sy = s.y;
        const float sz = s.z;
        const float fy = f.y;
        const float fz = f.z;
        const float uy = u.y;
        const float uz = u.z;
        s.y = sy * c - sz * sn;
        s.z = sy * sn + sz * c;
        f.y = fy * c - fz * sn;
        f.z = fy * sn + fz * c;
        u.y = uy * c - uz * sn;
        u.z = uy * sn + uz * c;
    }

    // 0x1402b6350: rotates basis vectors around global Y axis.
    void rotate_global_y(float angle) noexcept {
        const float c = std::cos(angle);
        const float sn = std::sin(angle);
        const float sx = s.x;
        const float sz = s.z;
        const float fx = f.x;
        const float fz = f.z;
        const float ux = u.x;
        const float uz = u.z;
        s.x = sz * sn + sx * c;
        s.z = sz * c - sx * sn;
        f.x = fz * sn + fx * c;
        f.z = fz * c - fx * sn;
        u.x = uz * sn + ux * c;
        u.z = uz * c - ux * sn;
    }

    // 0x1402b6420: rotates basis vectors around global Z axis.
    void rotate_global_z(float angle) noexcept {
        const float c = std::cos(angle);
        const float sn = std::sin(angle);
        const float sx = s.x;
        const float sy = s.y;
        const float fx = f.x;
        const float fy = f.y;
        const float ux = u.x;
        const float uy = u.y;
        s.x = sx * c - sy * sn;
        s.y = sx * sn + sy * c;
        f.x = fx * c - fy * sn;
        f.y = fx * sn + fy * c;
        u.x = ux * c - uy * sn;
        u.y = ux * sn + uy * c;
    }

    // 0x1402b69c0: copies orthonormal basis without changing origin o.
    void set_rotation(const WseTransform& other) noexcept {
        s = other.s;
        f = other.f;
        u = other.u;
    }

    // 0x1402b7010: transforms `other` from parent space into `this` local frame.
    [[nodiscard]] WseTransform transform_to_local(
        const WseTransform& other) const noexcept {
        WseTransform out{};
        const float dx = other.o.x - o.x;
        const float dy = other.o.y - o.y;
        const float dz = other.o.z - o.z;
        out.s.x = other.s.x * s.x + other.s.y * s.y + other.s.z * s.z;
        out.s.y = other.s.x * f.x + other.s.y * f.y + other.s.z * f.z;
        out.s.z = other.s.x * u.x + other.s.y * u.y + other.s.z * u.z;
        out.f.x = other.f.x * s.x + other.f.y * s.y + other.f.z * s.z;
        out.f.y = other.f.x * f.x + other.f.y * f.y + other.f.z * f.z;
        out.f.z = other.f.x * u.x + other.f.y * u.y + other.f.z * u.z;
        out.u.x = other.u.x * s.x + other.u.y * s.y + other.u.z * s.z;
        out.u.y = other.u.x * f.x + other.u.y * f.y + other.u.z * f.z;
        out.u.z = other.u.x * u.x + other.u.y * u.y + other.u.z * u.z;
        out.o.x = s.x * dx + s.y * dy + s.z * dz;
        out.o.y = f.x * dx + f.y * dy + f.z * dz;
        out.o.z = u.x * dx + u.y * dy + u.z * dz;
        return out;
    }

    // 0x1402b7330: transforms `local` from `this` frame into parent/world space.
    [[nodiscard]] WseTransform transform_to_parent(
        const WseTransform& local) const noexcept {
        WseTransform out{};
        out.s.x = s.x * local.s.x + f.x * local.s.y + u.x * local.s.z;
        out.s.y = s.y * local.s.x + f.y * local.s.y + u.y * local.s.z;
        out.s.z = s.z * local.s.x + f.z * local.s.y + u.z * local.s.z;
        out.f.x = s.x * local.f.x + f.x * local.f.y + u.x * local.f.z;
        out.f.y = s.y * local.f.x + f.y * local.f.y + u.y * local.f.z;
        out.f.z = s.z * local.f.x + f.z * local.f.y + u.z * local.f.z;
        out.u.x = s.x * local.u.x + f.x * local.u.y + u.x * local.u.z;
        out.u.y = s.y * local.u.x + f.y * local.u.y + u.y * local.u.z;
        out.u.z = s.z * local.u.x + f.z * local.u.y + u.z * local.u.z;
        out.o.x = s.x * local.o.x + f.x * local.o.y + u.x * local.o.z + o.x;
        out.o.y = s.y * local.o.x + f.y * local.o.y + u.y * local.o.z + o.y;
        out.o.z = s.z * local.o.x + f.z * local.o.y + u.z * local.o.z + o.z;
        return out;
    }

    // 0x1402b6720: normalize f, s = normalize(f x u), u = s x f.
    void orthonormalize() noexcept {
        f.normalize();
        s = f.cross(u);
        s.normalize();
        u = s.cross(f);
    }
};

// Default zeroed rglTransform for array_create / array_resize_dim (0-position).
[[nodiscard]] constexpr WseTransform wse_zero_transform() noexcept {
    return {{0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 0.0f, 0.0f}};
}

// Mirrors rglTransform compare in FUN_1401f5a20 (0x1401f619d): orthonormalizes
// copies of both transforms, then compares the 12 basis + origin floats.
[[nodiscard]] inline bool wse_transforms_equal(const WseTransform& lhs,
                                               const WseTransform& rhs) noexcept {
    WseTransform a = lhs;
    WseTransform b = rhs;
    a.orthonormalize();
    b.orthonormalize();
    return a.s.x == b.s.x && a.s.y == b.s.y && a.s.z == b.s.z &&
           a.f.x == b.f.x && a.f.y == b.f.y && a.f.z == b.f.z &&
           a.u.x == b.u.x && a.u.y == b.u.y && a.u.z == b.u.z &&
           a.o.x == b.o.x && a.o.y == b.o.y && a.o.z == b.o.z;
}


// Standard MT19937 PRNG mirroring rglMersenneTwister (0x1402a2760/0x1402a2850/0x1402a2920/0x1402a29f0).
struct WseMersenneTwister {
    std::array<std::uint32_t, 624> state{};
    std::size_t index = 624;

    WseMersenneTwister() noexcept {
        seed(5489U);
    }

    void seed(std::uint32_t value) noexcept {
        state[0] = value;
        for (std::size_t i = 1; i < state.size(); ++i) {
            const std::uint32_t prev = state[i - 1];
            state[i] = 0x6c078965U * (prev ^ (prev >> 30U)) +
                       static_cast<std::uint32_t>(i);
        }
        index = state.size();
    }

    [[nodiscard]] std::uint32_t next_u32() noexcept {
        if (index >= state.size()) {
            twist();
        }
        std::uint32_t y = state[index++];
        y ^= y >> 11U;
        y ^= (y << 7U) & 0x9d2c5680U;
        y ^= (y << 15U) & 0xefc60000U;
        y ^= y >> 18U;
        return y;
    }

    // Exact match for 0x1402a2850: uniform [min, max) using 32-bit unsigned span.
    [[nodiscard]] std::int32_t get_int(std::int32_t min,
                                       std::int32_t max) noexcept {
        const std::uint32_t b = static_cast<std::uint32_t>(max) - 1U;
        const std::uint32_t span = b - static_cast<std::uint32_t>(min);
        if (span == 0xffffffffU) {
            return static_cast<std::int32_t>(next_u32() +
                                             static_cast<std::uint32_t>(min));
        }
        const std::uint32_t range = span + 1U;
        const std::uint32_t q = 0xffffffffU / range;
        const std::uint32_t rem = 0xffffffffU % range;
        std::uint32_t r = 0U;
        if (span != 0U) {
            for (;;) {
                r = next_u32();
                if ((r / range) < q || rem == span) {
                    break;
                }
            }
        }
        return static_cast<std::int32_t>((r % range) +
                                         static_cast<std::uint32_t>(min));
    }

private:
    void twist() noexcept {
        constexpr std::size_t n = 624;
        constexpr std::size_t m = 397;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t y = (state[i] & 0x80000000U) |
                                    (state[(i + 1) % n] & 0x7fffffffU);
            state[i] = state[(i + m) % n] ^ (y >> 1U);
            if ((y & 1U) != 0U) {
                state[i] ^= 0x9908b0dfU;
            }
        }
        index = 0;
    }
};

// rglRound64 (0x1402a1b80): floorf(x), plus one when the fraction is > 0.5.
// Out-of-range values become INT64_MIN because the retail conversion is
// cvttss2si; this avoids undefined behaviour on the host.
[[nodiscard]] inline std::int64_t wse_round64(float value) noexcept {
    const float base = std::floor(value);
    if (!std::isfinite(base) ||
        base < static_cast<float>(std::numeric_limits<std::int64_t>::min()) ||
        base >= static_cast<float>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    auto result = static_cast<std::int64_t>(base);
    if (value - base > 0.5f) {
        ++result;
    }
    return result;
}

// cvttsd2si semantics: NaN and out-of-range values become INT64_MIN.
[[nodiscard]] inline std::int64_t wse_trunc64(double value) noexcept {
    if (!std::isfinite(value) ||
        value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}


}  // namespace wse::core
