// Android ADPF performance-hint session wrapper (API 33+) with thread-priority
// fallback for API 29-32 (docs/PLATFORM_PLAN.md, P10.8).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations. Loaded at run time via dlopen/dlsym so the
// binary builds and runs against minSdk 29. On non-Android hosts, inline no-op
// stand-ins are provided so callers (such as sdl_selftest_main.cpp) can invoke
// the API unconditionally without platform #ifdef clutter.

#pragma once

#include <cstdint>
#include <memory>

namespace wse::platform::android_perf {

#ifdef __ANDROID__

class PerformanceHintSession {
public:
    // Returns nullptr when the API is unavailable (pre-API 33 or unsupported HAL),
    // `target_frame_ns <= 0`, or session creation fails.
    [[nodiscard]] static std::unique_ptr<PerformanceHintSession> create(
        std::int64_t target_frame_ns);

    ~PerformanceHintSession();
    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    void report_actual(std::int64_t work_ns) noexcept;
    void update_target(std::int64_t target_frame_ns) noexcept;

private:
    explicit PerformanceHintSession(void* session) noexcept;

    void* session_ = nullptr;
};

// Fallback for pre-API 33 or failed session creation: sets current thread niceness
// to -4 (Android THREAD_PRIORITY_DISPLAY). Returns whether it succeeded; failure is
// non-fatal.
bool raise_current_thread_priority() noexcept;

#else

class PerformanceHintSession {
public:
    [[nodiscard]] static std::unique_ptr<PerformanceHintSession> create(
        std::int64_t /*target_frame_ns*/) {
        return nullptr;
    }

    ~PerformanceHintSession() = default;
    PerformanceHintSession(const PerformanceHintSession&) = delete;
    PerformanceHintSession& operator=(const PerformanceHintSession&) = delete;

    void report_actual(std::int64_t /*work_ns*/) noexcept {}
    void update_target(std::int64_t /*target_frame_ns*/) noexcept {}

private:
    PerformanceHintSession() = default;
};

inline bool raise_current_thread_priority() noexcept { return false; }

#endif

}  // namespace wse::platform::android_perf
