// Portable dispatch for WSE-extended statement opcodes.
//
// Evidence (Ghidra 12.1.4 pseudocode):
//   mbStatement::execute 0x1401c21e0 decodes every operand through
//     mbStatement::parseOperand (0x1401c2100) into three fixed 32-element
//     arrays: int64 values, int32 truncations, and 4-byte tags. It masks the
//     opcode with 0x0FFFFFFF, dispatches opcodes 30..2767 through the native
//     jump table at 0x1401de220, and calls executeWSE for everything else.
//   mbStatement::executeWSE 0x1401e0cf0 receives those arrays, computes a
//     64-bit result in a handler, then writes it through operand 0 with
//     mbStatement::returnValue (0x1401c2190). A handler returning false is a
//     failed condition; a handler returning true without using returnValue is
//     a successful no-op (for example an out-of-range register).
//   Opcode 2800 is an equality-tested handler at pseudocode line 2390 and
//     opcodes 2801..2809 are switch cases at lines 2165..2192.
//   The floating-point group starts at case 0x1130 (line 5369) and uses the
//     float array DAT_140649318, indexed by a register number. mbCheckRegister
//     0x1401c0ef0 accepts 0..127 (`cmp ecx,0x7f; ja`). DAT_1406137b8 is the
//     fixed-point scale and is set to 0x64 (100) at 0x140070f3a. fst uses
//     rglRound64 0x1402950d0: floorf(x) plus one when the fraction is > 0.5.
//     feq/fge/fle compare through rglAbs 0x140295010 with FLT_EPSILON.
//
// The complete 433-opcode structure is in docs/WSE_OPCODES.md and
// docs/wse_opcodes.json; wse_opcodes.h is generated from that map. This file
// implements the arithmetic/bitwise group 2800..2809 and the self-contained
// floating-point register group 4400, 4405..4417, and 4438. Every other known
// opcode reports WseDispatch::unimplemented instead of guessing.

#pragma once

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "wse/vm/wse_opcodes.h"

namespace wse::vm {

inline constexpr std::uint32_t kWseOpcodeMask = 0x0FFFFFFFu;

// mbCheckRegister accepts unsigned indices 0..0x7f.
inline constexpr std::size_t kFpRegisterCount = 128;

// DAT_1406137b8, fixed-point values per unit (set to 100 at 0x140070f3a).
inline constexpr float kWseFixedPointScale = 100.0f;

// Decoded operands, matching the arrays built by mbStatement::execute.
struct WseOperands {
    std::span<const std::int64_t> wide{};     // param_3, full int64 values
    std::span<const std::int32_t> narrow{};   // param_4, int32 truncations
    std::span<const std::uint8_t> tags{};     // param_5, parseOperand tags
    std::span<float> fp{};                    // DAT_140649318, 128 floats
};

// Result of a successful handler. Value-writing handlers (mbStatement's
// common LAB_1401e0e0e tail) write effect.value through operand 0; the
// floating-point and comparison handlers write their own destinations.
struct WseEffect {
    std::int64_t value = 0;
    bool store_value = true;
};

enum class WseDispatch : std::uint8_t {
    handled = 0,                 // success; write effect.value through operand 0
    handled_no_store,            // success; handler wrote its own destination
    condition_false,             // executeWSE returned false (failed condition)
    ignored,                     // success; retail returned true without effect
    unimplemented,
    operand_count_out_of_range,
};

[[nodiscard]] constexpr std::string_view describe(WseDispatch result) noexcept {
    switch (result) {
    case WseDispatch::handled:
        return "handled";
    case WseDispatch::handled_no_store:
        return "handled without operand-0 store";
    case WseDispatch::condition_false:
        return "false condition";
    case WseDispatch::ignored:
        return "ignored (invalid index)";
    case WseDispatch::unimplemented:
        return "unimplemented WSE opcode";
    case WseDispatch::operand_count_out_of_range:
        return "WSE opcode operand count out of range";
    }
    return "unknown WSE dispatch result";
}

// The shift amount is `(byte)operand & 0x3F`, exactly as the retail handlers
// use it (x86-64 masks a 64-bit shift by 63).
[[nodiscard]] constexpr unsigned wse_shift(std::int64_t value) noexcept {
    return static_cast<unsigned>(value & 0x3F);
}

// mbCheckRegister: unsigned comparison against 0x7f.
[[nodiscard]] constexpr bool wse_register_ok(std::int32_t index) noexcept {
    return static_cast<std::uint32_t>(index) <= 0x7Fu;
}

// rglRound64 (0x1402950d0): floorf(x), plus one when the fraction is > 0.5.
// Out-of-range values become INT64_MIN because the retail conversion is
// cvttss2si; this avoids undefined behaviour on the host.
[[nodiscard]] inline std::int64_t wse_round64(float value) noexcept {
    const float base = std::floor(value);
    if (!std::isfinite(base) ||
        base < static_cast<float>(std::numeric_limits<std::int64_t>::min()) ||
        base >= static_cast<float>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    auto result = static_cast<std::int64_t>(base);
    if (value - base > 0.5f) {
        ++result;
    }
    return result;
}

// `val_*` uses operand 0 as both source and destination; `store_*` reads
// operands 1.. and leaves operand 0 as the destination lvalue for the caller.
[[nodiscard]] inline WseDispatch execute_wse(std::uint32_t opcode,
                                             const WseOperands& operands,
                                             WseEffect& effect) noexcept {
    opcode &= kWseOpcodeMask;
    effect.store_value = false;
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    const auto fp = operands.fp;

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::val_shr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[0] >> wse_shift(wide[1]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_shr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[1] >> wse_shift(wide[2]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_lshr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) >> wse_shift(wide[1]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_lshr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) >> wse_shift(wide[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_shl: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) << wse_shift(wide[1]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_shl: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) << wse_shift(wide[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_xor: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[0] ^ wide[1];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_xor: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[1] ^ wide[2];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_not: {
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = ~wide[0];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_not: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = ~wide[1];
        effect.store_value = true;
        return WseDispatch::handled;
    }

    // Floating-point registers. The retail cases check every register before
    // doing anything; a failed check falls through to `return true`, which is
    // a successful no-op.
    case WseOpcode::fld: {
        if (narrow.size() < 1 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        fp[index] = static_cast<float>(wide[1]) / kWseFixedPointScale;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fst: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[1]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(kWseFixedPointScale * fp[index]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::fcpy: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto source = static_cast<std::size_t>(narrow[1]);
        if (destination >= fp.size() || source >= fp.size()) {
            return WseDispatch::ignored;
        }
        fp[destination] = fp[source];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::feq: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float difference = std::fabs(fp[lhs] - fp[rhs]);
        return difference <= FLT_EPSILON ? WseDispatch::handled_no_store
                                         : WseDispatch::condition_false;
    }
    case WseOpcode::fgt: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        return fp[rhs] <= fp[lhs] && fp[lhs] != fp[rhs]
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::flt: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        return fp[lhs] <= fp[rhs] && fp[lhs] != fp[rhs]
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::fge: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        bool result = true;
        if (a < b || a == b) {
            if (std::fabs(a - b) > FLT_EPSILON) {
                result = false;
            }
        }
        return result ? WseDispatch::handled_no_store
                      : WseDispatch::condition_false;
    }
    case WseOpcode::fle: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        bool result = true;
        if (b < a || b == a) {
            if (std::fabs(a - b) > FLT_EPSILON) {
                result = false;
            }
        }
        return result ? WseDispatch::handled_no_store
                      : WseDispatch::condition_false;
    }
    case WseOpcode::fsub:
    case WseOpcode::fmul:
    case WseOpcode::fdiv:
    case WseOpcode::fmin:
    case WseOpcode::fmax:
    case WseOpcode::fadd: {
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto lhs = static_cast<std::size_t>(narrow[1]);
        const auto rhs = static_cast<std::size_t>(narrow[2]);
        if (destination >= fp.size() || lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fsub:
            fp[destination] = a - b;
            break;
        case WseOpcode::fmul:
            fp[destination] = a * b;
            break;
        case WseOpcode::fdiv:
            if (b == 0.0f) {
                return WseDispatch::handled_no_store;
            }
            fp[destination] = a / b;
            break;
        case WseOpcode::fmin:
            fp[destination] = b <= a ? b : a;
            break;
        case WseOpcode::fmax:
            fp[destination] = a <= b ? b : a;
            break;
        case WseOpcode::fadd:
            fp[destination] = a + b;
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fclamp: {
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || !wse_register_ok(narrow[3]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto value = static_cast<std::size_t>(narrow[1]);
        const auto first = static_cast<std::size_t>(narrow[2]);
        const auto second = static_cast<std::size_t>(narrow[3]);
        if (destination >= fp.size() || value >= fp.size() || first >= fp.size() ||
            second >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float v = fp[value];
        const float bound_a = fp[first];
        const float bound_b = fp[second];
        const float low = bound_b <= bound_a ? bound_b : bound_a;
        const float high = bound_a <= bound_b ? bound_b : bound_a;
        fp[destination] = v < low ? low : (v <= high ? v : high);
        return WseDispatch::handled_no_store;
    }
    default:
        break;
    }
    return WseDispatch::unimplemented;
}

}  // namespace wse::vm
