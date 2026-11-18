// Android ADPF performance-hint session wrapper and thread-priority fallback
// (`platform/android/perf_hint.h`, P10.8).

#include "perf_hint.h"

#ifdef __ANDROID__

#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace wse::platform::android_perf {

namespace {

// Upper clamp (1 second) so pause/stall outliers never skew ADPF budgets.
constexpr std::int64_t kMaxDurationNs = 1000000000LL;

// Opaque NDK types from <android/performance_hint.h> (API 33+); declared here
// so the library compiles cleanly against ANDROID_PLATFORM=android-29.
struct APerformanceHintManager;
struct APerformanceHintSession;

using GetManagerFn = APerformanceHintManager* (*)();
using CreateSessionFn = APerformanceHintSession* (*)(APerformanceHintManager*,
                                                     const std::int32_t*,
                                                     std::size_t,
                                                     std::int64_t);
using ReportActualFn = int (*)(APerformanceHintSession*, std::int64_t);
using UpdateTargetFn = int (*)(APerformanceHintSession*, std::int64_t);
using CloseSessionFn = void (*)(APerformanceHintSession*);

struct PerfHintApi {
    void* lib = nullptr;
    GetManagerFn get_manager = nullptr;
    CreateSessionFn create_session = nullptr;
    ReportActualFn report_actual = nullptr;
    UpdateTargetFn update_target = nullptr;
    CloseSessionFn close_session = nullptr;
    bool available = false;
};

template <typename Fn>
[[nodiscard]] bool load_symbol(void* lib, const char* name, Fn& out) noexcept {
    out = nullptr;
    if (lib == nullptr) {
        return false;
    }
    void* sym = ::dlsym(lib, name);
    if (sym == nullptr) {
        (void)::dlerror();
        return false;
    }
    static_assert(sizeof(out) == sizeof(sym), "unexpected function pointer size");
    std::memcpy(&out, &sym, sizeof(out));
    return true;
}

[[nodiscard]] const PerfHintApi& get_perf_hint_api() noexcept {
    static const PerfHintApi api = []() noexcept {
        PerfHintApi out{};
        (void)::dlerror();
        out.lib = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (out.lib == nullptr) {
            (void)::dlerror();
            return out;
        }
        if (!load_symbol(out.lib, "APerformanceHint_getManager", out.get_manager) ||
            !load_symbol(out.lib, "APerformanceHint_createSession", out.create_session) ||
            !load_symbol(out.lib, "APerformanceHint_reportActualWorkDuration",
                         out.report_actual) ||
            !load_symbol(out.lib, "APerformanceHint_updateTargetWorkDuration",
                         out.update_target) ||
            !load_symbol(out.lib, "APerformanceHint_closeSession", out.close_session)) {
            ::dlclose(out.lib);
            out = PerfHintApi{};
            return out;
        }
        out.available = true;
        return out;
    }();
    return api;
}

[[nodiscard]] constexpr std::int64_t clamp_duration_ns(std::int64_t ns) noexcept {
    return ns > kMaxDurationNs ? kMaxDurationNs : ns;
}

}  // namespace

std::unique_ptr<PerformanceHintSession> PerformanceHintSession::create(
    std::int64_t target_frame_ns) {
    if (target_frame_ns <= 0) {
        return nullptr;
    }
    const PerfHintApi& api = get_perf_hint_api();
    if (!api.available) {
        return nullptr;
    }
    APerformanceHintManager* mgr = api.get_manager();
    if (mgr == nullptr) {
        return nullptr;
    }
    const pid_t raw_tid = ::gettid();
    if (raw_tid <= 0) {
        return nullptr;
    }
    const std::int32_t tid = static_cast<std::int32_t>(raw_tid);
    const std::int64_t clamped_ns = clamp_duration_ns(target_frame_ns);
    APerformanceHintSession* session = api.create_session(mgr, &tid, 1U, clamped_ns);
    if (session == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<PerformanceHintSession>(new PerformanceHintSession(session));
}

PerformanceHintSession::PerformanceHintSession(void* session) noexcept : session_(session) {}

PerformanceHintSession::~PerformanceHintSession() {
    if (session_ != nullptr) {
        const PerfHintApi& api = get_perf_hint_api();
        if (api.available && api.close_session != nullptr) {
            api.close_session(static_cast<APerformanceHintSession*>(session_));
        }
        session_ = nullptr;
    }
}

void PerformanceHintSession::report_actual(std::int64_t work_ns) noexcept {
    if (session_ == nullptr || work_ns <= 0) {
        return;
    }
    const PerfHintApi& api = get_perf_hint_api();
    if (api.available && api.report_actual != nullptr) {
        (void)api.report_actual(static_cast<APerformanceHintSession*>(session_),
                                clamp_duration_ns(work_ns));
    }
}

void PerformanceHintSession::update_target(std::int64_t target_frame_ns) noexcept {
    if (session_ == nullptr || target_frame_ns <= 0) {
        return;
    }
    const PerfHintApi& api = get_perf_hint_api();
    if (api.available && api.update_target != nullptr) {
        (void)api.update_target(static_cast<APerformanceHintSession*>(session_),
                                clamp_duration_ns(target_frame_ns));
    }
}

bool raise_current_thread_priority() noexcept {
    const pid_t tid = ::gettid();
    if (tid <= 0) {
        return false;
    }
    // Android Process.THREAD_PRIORITY_DISPLAY (-4); non-fatal if the OS denies raising niceness.
    return ::setpriority(PRIO_PROCESS, static_cast<id_t>(tid), -4) == 0;
}

}  // namespace wse::platform::android_perf

#endif  // __ANDROID__
