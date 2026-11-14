// Platform seam: monotonic time.
//
// Port infrastructure, not a reconstruction. The retail engine reads
// QueryPerformanceCounter and keeps its timers in 100,000-per-second ticks
// (trigger timers and `timer_get_elapsed_time`; see STATUS.md appendix A).
// Engine code takes a `Clock&` so tests can drive time by hand and the
// Android build can pause it with the activity lifecycle.

#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace wse::platform {

inline constexpr std::int64_t kTicksPerSecond = 100000;

inline void sleep_ms(std::int32_t ms) {
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

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

}  // namespace wse::platform
