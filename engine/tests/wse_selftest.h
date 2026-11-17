// Clean-room in-process runner for all portable engine test suites.
// Used both by host CTest verification and the Android SDL3 self-test shell
// (docs/PLATFORM_PLAN.md, slice P1).

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace wse::selftest {

int run_operand_suite(const char* fixture_path);
int run_trigger_suite(const char* fixture_path);
int run_savegame_suite(const char* fixture_path);
int run_script_suite(const char* fixture_path);
int run_wse_bitwise_fp_suite(const char* fixture_path);
int run_wse_system_suite(const char* fixture_path);
int run_wse_string_suite(const char* fixture_path);
int run_wse_dict_suite(const char* fixture_path);
int run_wse_array_suite(const char* fixture_path);
int run_wse_world_suite(const char* fixture_path);
int run_block_vm_suite(const char* fixture_path);
int run_native_world_suite(const char* fixture_path);
int run_native_m5_suite(const char* fixture_path);
int run_wse_m6_suite(const char* fixture_path);
int run_platform_suite(const char* fixture_path);
int run_module_loaders_suite(const char* fixture_path);
int run_headless_suite(const char* fixture_path);
int run_lua_suite(const char* fixture_path);
int run_asset_suite(const char* fixture_path);
int run_audio_suite(const char* fixture_path);
int run_input_suite(const char* fixture_path);
int run_scene_suite(const char* fixture_path);
int run_ui_suite(const char* fixture_path);

struct SuiteResult {
    const char* name = "";
    bool passed = false;
    std::int64_t duration_ms = 0;
};

struct SuiteSummary {
    std::vector<SuiteResult> results;
    int passed_count = 0;
    int total_count = 0;
    std::int64_t total_ms = 0;
    // True when `should_stop` ended the run before every suite had started.
    bool cancelled = false;

    [[nodiscard]] bool all_passed() const noexcept {
        return !cancelled && passed_count == total_count && total_count > 0;
    }
};

// Called right before a suite starts (completed_or_null == nullptr) and
// right after it finishes (completed_or_null != nullptr).
using ProgressCallback = std::function<void(int index, int total, const char* suite_name,
                                            const SuiteResult* completed_or_null)>;

// Polled before each suite starts; returning true abandons the remaining
// suites (a suite that is already running always finishes).
using StopCallback = std::function<bool()>;

SuiteSummary run_all_suites(const char* scripts_fixture_path,
                            const ProgressCallback& progress = {},
                            const StopCallback& should_stop = {});

}  // namespace wse::selftest
