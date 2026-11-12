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
//     mbStatement::returnValue (0x1401c2190).
//   Opcode 2800 is an equality-tested handler at pseudocode line 2390 and
//     opcodes 2801..2809 are switch cases at lines 2165..2192.
//
// The complete 433-opcode structure is in docs/WSE_OPCODES.md and
// docs/wse_opcodes.json; wse_opcodes.h is generated from that map. This file
// starts with the arithmetic/bitwise group 2800..2809, whose bodies are small
// enough to transcribe directly. Every other known opcode reports
// WseDispatch::unimplemented instead of guessing.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "wse/vm/wse_opcodes.h"

namespace wse::vm {

inline constexpr std::uint32_t kWseOpcodeMask = 0x0FFFFFFFu;

// Decoded operands, matching the arrays built by mbStatement::execute.
struct WseOperands {
    std::span<const std::int64_t> wide{};     // param_3, full int64 values
    std::span<const std::int32_t> narrow{};   // param_4, int32 truncations
    std::span<const std::uint8_t> tags{};     // param_5, parseOperand tags
};

enum class WseDispatch : std::uint8_t {
    handled = 0,
    unimplemented,
    operand_count_out_of_range,
};

[[nodiscard]] constexpr std::string_view describe(WseDispatch result) noexcept {
    switch (result) {
    case WseDispatch::handled:
        return "handled";
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

// `val_*` uses operand 0 as both source and destination; `store_*` reads
// operands 1.. and leaves operand 0 as the destination lvalue for the caller.
[[nodiscard]] inline WseDispatch execute_wse(std::uint32_t opcode,
                                             const WseOperands& operands,
                                             std::int64_t& result) noexcept {
    opcode &= kWseOpcodeMask;
    const auto wide = operands.wide;

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::val_shr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = wide[0] >> wse_shift(wide[1]);
        return WseDispatch::handled;
    }
    case WseOpcode::store_shr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = wide[1] >> wse_shift(wide[2]);
        return WseDispatch::handled;
    }
    case WseOpcode::val_lshr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) >> wse_shift(wide[1]));
        return WseDispatch::handled;
    }
    case WseOpcode::store_lshr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) >> wse_shift(wide[2]));
        return WseDispatch::handled;
    }
    case WseOpcode::val_shl: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) << wse_shift(wide[1]));
        return WseDispatch::handled;
    }
    case WseOpcode::store_shl: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) << wse_shift(wide[2]));
        return WseDispatch::handled;
    }
    case WseOpcode::val_xor: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = wide[0] ^ wide[1];
        return WseDispatch::handled;
    }
    case WseOpcode::store_xor: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = wide[1] ^ wide[2];
        return WseDispatch::handled;
    }
    case WseOpcode::val_not: {
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = ~wide[0];
        return WseDispatch::handled;
    }
    case WseOpcode::store_not: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        result = ~wide[1];
        return WseDispatch::handled;
    }
    default:
        break;
    }
    return WseDispatch::unimplemented;
}

}  // namespace wse::vm
