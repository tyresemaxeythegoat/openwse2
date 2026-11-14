// Platform seam: monotonic time and wall-clock time.
//
// Port infrastructure, not a reconstruction. The retail engine reads
// QueryPerformanceCounter and keeps its timers in 100,000-per-second ticks
// (trigger timers and `timer_get_elapsed_time`; see STATUS.md appendix A).
// Engine code takes a `Clock&` so tests can drive time by hand and the
// Android build can pause it with the activity lifecycle. Calendar time
// (`get_time`) goes through `WallClock` for the same reason. Blocking sleeps
// live in wse/platform/sleep.h.

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>

namespace wse::platform {

inline constexpr std::int64_t kTicksPerSecond = 100000;

class Clock {
public:
    virtual ~Clock() = default;

    // Monotonic ticks since an arbitrary epoch, kTicksPerSecond per second.
    virtual std::int64_t ticks() = 0;

    double seconds() { return static_cast<double>(ticks()) / static_cast<double>(kTicksPerSecond); }
};

class SteadyClock final : public Clock {
public:
    std::int64_t ticks() override {
        const auto elapsed = std::chrono::steady_clock::now() - start_;
        return std::chrono::duration_cast<
                   std::chrono::duration<std::int64_t, std::ratio<1, kTicksPerSecond>>>(elapsed)
            .count();
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Deterministic clock for tests and for a paused application.
class ManualClock final : public Clock {
public:
    std::int64_t ticks() override { return ticks_; }
    void set_ticks(std::int64_t ticks) noexcept { ticks_ = ticks; }
    void advance_ticks(std::int64_t delta) noexcept { ticks_ += delta; }
    void advance_seconds(double seconds) noexcept {
        ticks_ += static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond));
    }

private:
    std::int64_t ticks_ = 0;
};

// Calendar time. Not monotonic: the user or the network can move it.
class WallClock {
public:
    virtual ~WallClock() = default;

    // Seconds since 1970-01-01T00:00:00Z.
    virtual std::int64_t unix_seconds() = 0;

    // Seconds to add to the UTC instant `unix_seconds` to get local civil
    // time, daylight saving included. Zero when the zone cannot be read.
    virtual std::int32_t utc_offset_seconds(std::int64_t unix_seconds) = 0;
};

class SystemWallClock final : public WallClock {
public:
    std::int64_t unix_seconds() override {
        return static_cast<std::int64_t>(std::time(nullptr));
    }

    std::int32_t utc_offset_seconds(std::int64_t unix_seconds) override {
        const std::time_t utc = static_cast<std::time_t>(unix_seconds);
        std::tm local_tm{};
#ifdef _WIN32
        if (localtime_s(&local_tm, &utc) != 0) return 0;
        const std::time_t as_utc = _mkgmtime(&local_tm);
#else
        if (localtime_r(&utc, &local_tm) == nullptr) return 0;
        const std::time_t as_utc = timegm(&local_tm);
#endif
        if (as_utc == static_cast<std::time_t>(-1)) return 0;
        return static_cast<std::int32_t>(static_cast<std::int64_t>(as_utc) - unix_seconds);
    }
};

// Deterministic wall clock for tests.
class ManualWallClock final : public WallClock {
public:
    std::int64_t unix_seconds() override { return unix_seconds_; }
    std::int32_t utc_offset_seconds(std::int64_t) override { return utc_offset_seconds_; }
    void set(std::int64_t unix_seconds, std::int32_t utc_offset_seconds = 0) noexcept {
        unix_seconds_ = unix_seconds;
        utc_offset_seconds_ = utc_offset_seconds;
    }

private:
    std::int64_t unix_seconds_ = 0;
    std::int32_t utc_offset_seconds_ = 0;
};

}  // namespace wse::platform
