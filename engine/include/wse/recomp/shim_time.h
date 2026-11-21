// Win64 time and counters for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design",
// family "Time and counters"; v1.1.5.6 call sites: QueryPerformanceCounter 110,
// GetSystemTimeAsFileTime 21, GetTickCount 15, timeGetTime and friends 70):
//
//   QueryPerformanceCounter QueryPerformanceFrequency GetTickCount GetTickCount64
//   GetSystemTimeAsFileTime GetSystemTime GetLocalTime GetTimeZoneInformation
//   SystemTimeToFileTime FileTimeToSystemTime FileTimeToLocalFileTime
//   SystemTimeToTzSpecificLocalTime            (KERNEL32)
//   timeGetTime timeBeginPeriod timeEndPeriod  (WINMM)
//
// Counters come from the host's monotonic clock (a 10 MHz performance counter: the frequency is
// machine-dependent on Windows, 24 MHz on the PC this was checked on, and programs read it);
// wall-clock time from the host's system clock as a FILETIME (100 ns since 1601 UTC).
// The local time zone is a fixed bias, taken from the host when the object is built (or set by
// the embedder); daylight-saving changes are not modelled. Header-only.

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

inline constexpr std::int64_t kQpcFrequency = 10'000'000;
inline constexpr std::int64_t kFileTimeUnixEpoch = 116444736000000000LL;  // 1970-01-01 in FILETIME
inline constexpr std::uint32_t kTimeZoneIdUnknown = 0;

namespace time_detail {
template <class T>
T get(std::uint64_t va) noexcept {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(va), sizeof(T));
    return v;
}
template <class T>
void put(std::uint64_t va, T v) noexcept {
    std::memcpy(reinterpret_cast<void*>(va), &v, sizeof(T));
}
}  // namespace time_detail

// SYSTEMTIME (8 WORDs).
struct SystemTime {
    std::uint16_t year = 1601, month = 1, day_of_week = 1, day = 1, hour = 0, minute = 0, second = 0, ms = 0;
};

// Days since 1970-01-01 for a proleptic Gregorian date, and back (Howard Hinnant's algorithms).
inline std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) noexcept {
    y -= m <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}
inline void civil_from_days(std::int64_t z, std::int64_t& y, unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2 ? 1 : 0;
}

// FILETIME <-> SYSTEMTIME (FileTimeToSystemTime / SystemTimeToFileTime). False when out of range.
inline bool filetime_to_systemtime(std::uint64_t ft, SystemTime& st) noexcept {
    if (ft >= 0x8000000000000000ULL) {
        return false;
    }
    const std::int64_t unix_100ns = static_cast<std::int64_t>(ft) - kFileTimeUnixEpoch;
    std::int64_t days = unix_100ns / 864000000000LL;
    std::int64_t rem = unix_100ns % 864000000000LL;
    if (rem < 0) {
        rem += 864000000000LL;
        --days;
    }
    std::int64_t y = 0;
    unsigned m = 0;
    unsigned d = 0;
    civil_from_days(days, y, m, d);
    st.year = static_cast<std::uint16_t>(y);
    st.month = static_cast<std::uint16_t>(m);
    st.day = static_cast<std::uint16_t>(d);
    st.day_of_week = static_cast<std::uint16_t>(((days % 7) + 7 + 4) % 7);  // 1970-01-01 was a Thursday
    const std::int64_t ms_of_day = rem / 10000;
    st.hour = static_cast<std::uint16_t>(ms_of_day / 3600000);
    st.minute = static_cast<std::uint16_t>(ms_of_day / 60000 % 60);
    st.second = static_cast<std::uint16_t>(ms_of_day / 1000 % 60);
    st.ms = static_cast<std::uint16_t>(ms_of_day % 1000);
    return true;
}
inline bool systemtime_to_filetime(const SystemTime& st, std::uint64_t& ft) noexcept {
    static constexpr unsigned kDays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (st.year < 1601 || st.year > 30827 || st.month < 1 || st.month > 12 || st.day < 1 ||
        st.day > kDays[st.month - 1] || st.hour > 23 || st.minute > 59 || st.second > 59 || st.ms > 999) {
        return false;
    }
    const bool leap = (st.year % 4 == 0 && st.year % 100 != 0) || st.year % 400 == 0;
    if (st.month == 2 && st.day == 29 && !leap) {
        return false;
    }
    const std::int64_t days = days_from_civil(st.year, st.month, st.day);
    const std::int64_t ms = ((static_cast<std::int64_t>(st.hour) * 60 + st.minute) * 60 + st.second) * 1000 + st.ms;
    ft = static_cast<std::uint64_t>((days * 86400000LL + ms) * 10000 + kFileTimeUnixEpoch);
    return true;
}
inline SystemTime read_systemtime(std::uint64_t va) noexcept {
    SystemTime st;
    std::memcpy(&st, reinterpret_cast<const void*>(va), sizeof(st));
    return st;
}
inline void write_systemtime(std::uint64_t va, const SystemTime& st) noexcept {
    std::memcpy(reinterpret_cast<void*>(va), &st, sizeof(st));
}

// The host's offset of local time from UTC in minutes (east positive), now.
inline std::int32_t host_utc_offset_minutes() noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    std::tm local{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
    localtime_s(&local, &now);
#else
    gmtime_r(&now, &utc);
    localtime_r(&now, &local);
#endif
    const std::int64_t d = days_from_civil(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                                           static_cast<unsigned>(local.tm_mday)) -
                           days_from_civil(utc.tm_year + 1900, static_cast<unsigned>(utc.tm_mon + 1),
                                           static_cast<unsigned>(utc.tm_mday));
    return static_cast<std::int32_t>(d * 1440 + (local.tm_hour - utc.tm_hour) * 60 + (local.tm_min - utc.tm_min));
}

class Time {
public:
    explicit Time(Runtime& rt) : rt_(rt) {}

    // Local time = UTC + this (minutes; Windows' `Bias` is its negation).
    std::int32_t utc_offset_minutes = host_utc_offset_minutes();

    [[nodiscard]] static std::uint64_t filetime_now() noexcept {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
        return static_cast<std::uint64_t>(ns / 100 + kFileTimeUnixEpoch);
    }
    // Monotonic 100 ns ticks (the performance counter).
    [[nodiscard]] static std::int64_t counter_now() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* dll, const char* name, Runtime::ImportShim shim) {
            n += rt_.set_import(dll, name, shim);
            if (std::strcmp(dll, "KERNEL32.dll") == 0) {
                n += rt_.set_import("KERNELBASE.dll", name, shim);
            }
        };
        using time_detail::put;
        bind("KERNEL32.dll", "QueryPerformanceCounter", [](CpuState& s) {
            put<std::int64_t>(win64_arg(s, 0), counter_now());
            win64_return(s, 1);
        });
        bind("KERNEL32.dll", "QueryPerformanceFrequency", [](CpuState& s) {
            put<std::int64_t>(win64_arg(s, 0), kQpcFrequency);
            win64_return(s, 1);
        });
        bind("KERNEL32.dll", "GetTickCount", [](CpuState& s) {
            win64_return(s, static_cast<std::uint32_t>(counter_now() / 10000));
        });
        bind("KERNEL32.dll", "GetTickCount64", [](CpuState& s) {
            win64_return(s, static_cast<std::uint64_t>(counter_now() / 10000));
        });
        bind("WINMM.dll", "timeGetTime", [](CpuState& s) {
            win64_return(s, static_cast<std::uint32_t>(counter_now() / 10000));
        });
        bind("WINMM.dll", "timeBeginPeriod", [](CpuState& s) { win64_return(s, 0); });  // TIMERR_NOERROR
        bind("WINMM.dll", "timeEndPeriod", [](CpuState& s) { win64_return(s, 0); });
        bind("KERNEL32.dll", "GetSystemTimeAsFileTime", [](CpuState& s) {
            put<std::uint64_t>(win64_arg(s, 0), filetime_now());
            win64_return(s, 0);
        });
        bind("KERNEL32.dll", "GetSystemTime", [](CpuState& s) {
            SystemTime st;
            (void)filetime_to_systemtime(filetime_now(), st);
            write_systemtime(win64_arg(s, 0), st);
            win64_return(s, 0);
        });
        bind("KERNEL32.dll", "GetLocalTime", [this](CpuState& s) {
            SystemTime st;
            (void)filetime_to_systemtime(to_local(filetime_now()), st);
            write_systemtime(win64_arg(s, 0), st);
            win64_return(s, 0);
        });
        bind("KERNEL32.dll", "GetTimeZoneInformation", [this](CpuState& s) {
            // TIME_ZONE_INFORMATION (172 bytes): Bias, then names and DST rules (none).
            const std::uint64_t tz = win64_arg(s, 0);
            std::memset(reinterpret_cast<void*>(tz), 0, 172);
            put<std::int32_t>(tz, -utc_offset_minutes);
            win64_return(s, kTimeZoneIdUnknown);
        });
        bind("KERNEL32.dll", "SystemTimeToFileTime", [](CpuState& s) {
            std::uint64_t ft = 0;
            if (!systemtime_to_filetime(read_systemtime(win64_arg(s, 0)), ft)) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                win64_return(s, 0);
                return;
            }
            put<std::uint64_t>(win64_arg(s, 1), ft);
            win64_return(s, 1);
        });
        bind("KERNEL32.dll", "FileTimeToSystemTime", [](CpuState& s) {
            SystemTime st;
            if (!filetime_to_systemtime(time_detail::get<std::uint64_t>(win64_arg(s, 0)), st)) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                win64_return(s, 0);
                return;
            }
            write_systemtime(win64_arg(s, 1), st);
            win64_return(s, 1);
        });
        bind("KERNEL32.dll", "FileTimeToLocalFileTime", [this](CpuState& s) {
            put<std::uint64_t>(win64_arg(s, 1), to_local(time_detail::get<std::uint64_t>(win64_arg(s, 0))));
            win64_return(s, 1);
        });
        // SystemTimeToTzSpecificLocalTime(timeZone, *utc, *local): a null zone is the current one.
        // Checked on Windows: 1601-01-01 with a positive Bias (local < 1601) and overflows past
        // 0x7FFFFFFFFFFFFFFF fail with ERROR_INVALID_PARAMETER; valid conversions leave LastError
        // untouched.
        bind("KERNEL32.dll", "SystemTimeToTzSpecificLocalTime", [this](CpuState& s) {
            const std::uint64_t tz = win64_arg(s, 0);
            const std::int32_t offset = tz != 0 ? -time_detail::get<std::int32_t>(tz) : utc_offset_minutes;
            std::uint64_t ft = 0;
            SystemTime st;
            if (!systemtime_to_filetime(read_systemtime(win64_arg(s, 1)), ft)) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                win64_return(s, 0);
                return;
            }
            const std::int64_t delta = std::int64_t{offset} * 600000000LL;
            const bool out_of_range =
                (delta < 0 && ft < static_cast<std::uint64_t>(-delta)) ||
                (delta > 0 && ft > 0x7FFFFFFFFFFFFFFFULL - static_cast<std::uint64_t>(delta));
            if (out_of_range || !filetime_to_systemtime(ft + static_cast<std::uint64_t>(delta), st)) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                win64_return(s, 0);
                return;
            }
            write_systemtime(win64_arg(s, 2), st);
            win64_return(s, 1);
        });
        return n;
    }

private:
    [[nodiscard]] std::uint64_t to_local(std::uint64_t ft) const noexcept {
        return ft + static_cast<std::uint64_t>(std::int64_t{utc_offset_minutes} * 600000000LL);
    }

    Runtime& rt_;
};

}  // namespace wse::recomp::shim
