// R0 feasibility spikes (`docs/RECOMP_PLAN.md` "Slices"): (a) a shared object generated and
// linked in the app loads and runs, (b) memory maps at the image base `0x140000000`, (c) the
// translator's speed (and the whole-exe time extrapolated from it), (d) one retail function
// translated from the user's exe returns the same results as on the PC.
//
// Runs on Android (the launcher's Advanced > "R0 spikes" row) and on Linux hosts (CI, with
// synthetic code only: `exe` stays empty there). Returns a plain-text report.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wse::recomp {

struct R0SpikeInput {
    std::string work_dir;              // writable, app-private; spike files go in `<work_dir>/r0`
    std::vector<std::uint8_t> exe;     // the user's `mb_warband_wse2_x64.exe`, or empty
    std::string exe_sha256;            // lowercase hex, computed by the caller
    std::string exe_error;             // why `exe` is empty, for the report
    std::uint64_t apk_bytes = 0;       // 0 when unknown
    std::size_t synthetic_blocks = 2000;
};

struct R0SpikeResult {
    std::string report;
    bool a_loads = false;
    bool b_maps = false;
    bool c_measured = false;
    bool d_matches = false;
};

R0SpikeResult run_r0_spikes(const R0SpikeInput& input);

// SHA-256 of the pinned v1.1.5.5 exe (`docs/RECOMP_PLAN.md` "The target").
inline constexpr const char* kWse2V1155ExeSha256 =
    "d4e97d56981a6dab3de0348e3a0513ae200e7b5701b86732a4654b0aa4e29709";
// FNV-1a hash of `rglString::indexOf` over the `r0_inputs.h` cases, measured on the PC with
// `tools/r0_pc_reference.cpp` (native call == interpreter == model, 2026-11-18).
inline constexpr std::uint64_t kR0IndexOfPcHash = 0x7aeac64f5cbff537ULL;

}  // namespace wse::recomp
