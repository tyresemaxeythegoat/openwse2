#include "wse_selftest.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace wse::selftest {

namespace {

struct SuiteEntry {
    const char* name;
    int (*fn)(const char*);
};

constexpr std::array<SuiteEntry, 23> kSuites = {{
    {"operand", &run_operand_suite},
    {"trigger", &run_trigger_suite},
    {"savegame", &run_savegame_suite},
    {"script", &run_script_suite},
    {"wse_bitwise_fp", &run_wse_bitwise_fp_suite},
    {"wse_system", &run_wse_system_suite},
    {"wse_string", &run_wse_string_suite},
    {"wse_dict", &run_wse_dict_suite},
    {"wse_array", &run_wse_array_suite},
    {"wse_world", &run_wse_world_suite},
    {"block_vm", &run_block_vm_suite},
    {"native_world", &run_native_world_suite},
    {"native_m5", &run_native_m5_suite},
    {"wse_m6", &run_wse_m6_suite},
    {"platform", &run_platform_suite},
    {"module_loaders", &run_module_loaders_suite},
    {"headless", &run_headless_suite},
    {"lua", &run_lua_suite},
    {"asset", &run_asset_suite},
    {"audio", &run_audio_suite},
    {"input", &run_input_suite},
    {"scene", &run_scene_suite},
    {"ui", &run_ui_suite},
}};

}  // namespace

SuiteSummary run_all_suites(const char* scripts_fixture_path,
                            const ProgressCallback& progress,
                            const StopCallback& should_stop) {
    SuiteSummary summary;
    summary.total_count = static_cast<int>(kSuites.size());
    summary.results.reserve(kSuites.size());

    const auto total_start = std::chrono::steady_clock::now();
    for (int i = 0; i < summary.total_count; ++i) {
        const SuiteEntry& entry = kSuites[static_cast<std::size_t>(i)];
        if (should_stop && should_stop()) {
            summary.cancelled = true;
            break;
        }
        if (progress) {
            progress(i, summary.total_count, entry.name, nullptr);
        }

        const auto start = std::chrono::steady_clock::now();
        const int rc = entry.fn(scripts_fixture_path);
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();

        SuiteResult res;
        res.name = entry.name;
        res.passed = (rc == 0);
        res.duration_ms = static_cast<std::int64_t>(elapsed_ms);
        if (res.passed) {
            ++summary.passed_count;
        }
        summary.results.push_back(res);

        if (progress) {
            progress(i, summary.total_count, entry.name, &summary.results.back());
        }
    }
    summary.total_ms = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - total_start)
            .count());
    return summary;
}

}  // namespace wse::selftest
