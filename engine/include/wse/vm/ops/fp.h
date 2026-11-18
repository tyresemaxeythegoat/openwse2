#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "wse/vm/ops/fp_expr.h"
#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline WseDispatch execute_wse_fp(std::uint32_t opcode,
                                                     const WseOperands& operands,
                                                     WseEffect& effect) {
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    const auto tags = operands.tags;
    const auto fp = operands.fp;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    // Floating-point group (4400..4437 / 0x1130..0x1155).
    // Floating-point registers check `mbCheckRegister`; invalid indices
    // return without doing anything; a failed check falls through to
    // `return true`, which is a successful no-op.
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
    case WseOpcode::fld_str: {
        // 4401 (0x1131, 0x1401f0247): atof (0x14049975c) of operand 1 stored as
        // float in fp[operand 0].
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        fp[index] = static_cast<float>(
            wse_wtof(wse_resolve_string(narrow[1], tag, strings)));
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fld_pos_x:
    case WseOpcode::fld_pos_y:
    case WseOpcode::fld_pos_z: {
        // 4402..4404 (0x1132..0x1134, 0x1401f02d7..0x1401f037f):
        // copies pos[op1].o.{x,y,z} into fp[op0].
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            fp.empty() || positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto fp_idx = static_cast<std::size_t>(narrow[0]);
        const auto pos_idx = static_cast<std::size_t>(narrow[1]);
        if (fp_idx >= fp.size() || pos_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        const WseVec4& origin = positions[pos_idx].o;
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fld_pos_x:
            fp[fp_idx] = origin.x;
            break;
        case WseOpcode::fld_pos_y:
            fp[fp_idx] = origin.y;
            break;
        case WseOpcode::fld_pos_z:
            fp[fp_idx] = origin.z;
            break;
        default:
            break;
        }
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

    // Transcendental unary register operations: operand 0 is the destination,
    // operand 1 the source. All register checks happen before evaluation, and
    // a failed check is the retail `return true` no-op.
    case WseOpcode::fsqrt:
    case WseOpcode::fabs:
    case WseOpcode::fceil:
    case WseOpcode::ffloor:
    case WseOpcode::fexp:
    case WseOpcode::fln:
    case WseOpcode::fsin:
    case WseOpcode::fcos:
    case WseOpcode::ftan:
    case WseOpcode::fsinh:
    case WseOpcode::fcosh:
    case WseOpcode::ftanh:
    case WseOpcode::fasin:
    case WseOpcode::facos:
    case WseOpcode::fatan: {
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
        const float value = fp[source];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fsqrt:
            fp[destination] = std::sqrt(value);
            break;
        case WseOpcode::fabs:
            fp[destination] = std::fabs(value);
            break;
        case WseOpcode::fceil:
            fp[destination] = std::ceil(value);
            break;
        case WseOpcode::ffloor:
            fp[destination] = std::floor(value);
            break;
        case WseOpcode::fexp:
            fp[destination] = std::exp(value);
            break;
        case WseOpcode::fln:
            fp[destination] = std::log(value);
            break;
        case WseOpcode::fsin:
            fp[destination] = std::sin(value);
            break;
        case WseOpcode::fcos:
            fp[destination] = std::cos(value);
            break;
        case WseOpcode::ftan:
            fp[destination] = std::tan(value);
            break;
        case WseOpcode::fsinh:
            fp[destination] = std::sinh(value);
            break;
        case WseOpcode::fcosh:
            fp[destination] = std::cosh(value);
            break;
        case WseOpcode::ftanh:
            fp[destination] = std::tanh(value);
            break;
        case WseOpcode::fasin:
            fp[destination] = std::asin(value);
            break;
        case WseOpcode::facos:
            fp[destination] = std::acos(value);
            break;
        case WseOpcode::fatan:
            fp[destination] = std::atan(value);
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }

    // Binary register operations. xmm0 carries operand 1, xmm1 operand 2:
    // pow(base, exponent), atan2(y, x), and fmod(dividend, divisor).
    case WseOpcode::fpow:
    case WseOpcode::fmod:
    case WseOpcode::fatan2: {
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto lhs_index = static_cast<std::size_t>(narrow[1]);
        const auto rhs_index = static_cast<std::size_t>(narrow[2]);
        if (destination >= fp.size() || lhs_index >= fp.size() ||
            rhs_index >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float lhs = fp[lhs_index];
        const float rhs = fp[rhs_index];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fpow:
            fp[destination] = std::pow(lhs, rhs);
            break;
        case WseOpcode::fmod:
            if (rhs == 0.0f) {
                return WseDispatch::handled_no_store;
            }
            fp[destination] = std::fmod(lhs, rhs);
            break;
        case WseOpcode::fatan2:
            fp[destination] = std::atan2(lhs, rhs);
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }

    case WseOpcode::feval: {
        // 4437 (0x1155, 0x1401e4f40 L8353..8365): (expression_string).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string expr;
        if (!wse_resolve_parsed_utf8_operand(0, operands, expr)) {
            return WseDispatch::ignored;
        }
        (void)wse_eval_float_expr(expr, fp, operands.state.positions);
        return WseDispatch::handled_no_store;
    }

    // flog: log10(fp[operand 1]) / log10(base). The base is the int32 literal
    // in operand 2; with fewer than three operands the retail default is 10.
    case WseOpcode::flog: {
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
        std::int32_t base = 10;
        if (narrow.size() >= 3) {
            base = narrow[2];
        }
        if (base < 2) {
            return WseDispatch::handled_no_store;  // "invalid logarithm base %d"
        }
        fp[destination] =
            std::log10(fp[source]) / std::log10(static_cast<float>(base));
        return WseDispatch::handled_no_store;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
