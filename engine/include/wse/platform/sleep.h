// Platform seam: blocking sleep.
//
// Port infrastructure, not a reconstruction. Kept apart from clock.h so that
// only the one caller that blocks (`sleep_ms`, opcode 3026) pulls in
// `<thread>`. An embedder that must not stall its thread installs
// `WseVmState::sleep_ms_fn` instead; this is the fallback.

#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace wse::platform {

// Blocks the calling thread. Zero and negative durations return at once.
inline void sleep_ms(std::int32_t ms) {
    if (ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace wse::platform
