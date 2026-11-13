// Platform seam: diagnostics sink.
//
// Port infrastructure, not a reconstruction. Retail diagnostics go to
// `rgl_log.txt` and message boxes; portable code writes to a `LogSink` so the
// host prints to stderr, tests capture lines, and Android forwards to logcat
// and the on-device self-test screen (docs/PLATFORM_PLAN.md, slice P1).

#pragma once

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace wse::platform {

enum class LogLevel { debug, info, warning, error };

constexpr const char* log_level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug:   return "debug";
        case LogLevel::info:    return "info";
        case LogLevel::warning: return "warning";
        case LogLevel::error:   return "error";
    }
    return "?";
}

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(LogLevel level, std::string_view message) = 0;

    void debug(std::string_view message) { write(LogLevel::debug, message); }
    void info(std::string_view message) { write(LogLevel::info, message); }
    void warning(std::string_view message) { write(LogLevel::warning, message); }
    void error(std::string_view message) { write(LogLevel::error, message); }
};

class StderrLog final : public LogSink {
public:
    void write(LogLevel level, std::string_view message) override {
        std::fprintf(stderr, "[%s] %.*s\n", log_level_name(level),
                     static_cast<int>(message.size()), message.data());
    }
};

class CaptureLog final : public LogSink {
public:
    struct Line {
        LogLevel level;
        std::string message;
    };

    void write(LogLevel level, std::string_view message) override {
        lines.push_back({level, std::string(message)});
    }

    std::vector<Line> lines;
};

class NullLog final : public LogSink {
public:
    void write(LogLevel, std::string_view) override {}
};

}  // namespace wse::platform
