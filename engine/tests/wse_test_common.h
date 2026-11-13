#pragma once

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/vm/execute_wse.h"

namespace wse::test {

inline int failures = 0;

inline void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

[[nodiscard]] inline bool close(float lhs, float rhs,
                                float tolerance = 1e-5f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

using wse::vm::WseDispatch;
using wse::vm::WseEffect;
using wse::vm::WseOpcode;
using wse::vm::WseOperands;
using wse::vm::WseVmState;

[[nodiscard]] constexpr std::uint32_t opcode(WseOpcode value) noexcept {
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] inline WseDispatch run(
    std::uint32_t value,
    std::initializer_list<std::int64_t> values,
    std::initializer_list<std::int32_t> registers,
    WseEffect& effect,
    std::span<float> fp = {},
    std::span<std::int64_t> bank = {},
    WseVmState state = {},
    std::initializer_list<std::uint8_t> tags = {}) {
    WseOperands operands;
    operands.wide = std::span<const std::int64_t>(values.begin(), values.size());
    operands.narrow =
        std::span<const std::int32_t>(registers.begin(), registers.size());
    operands.tags = std::span<const std::uint8_t>(tags.begin(), tags.size());
    operands.fp = fp;
    operands.registers = bank;
    operands.state = state;
    effect.value = 0x1234;
    effect.store_value = true;
    return wse::vm::execute_wse(value, operands, effect);
}

}  // namespace wse::test
