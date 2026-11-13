#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline WseDispatch execute_wse_position(std::uint32_t opcode,
                                                     const WseOperands& operands,
                                                     WseEffect& effect) {
    const auto narrow = operands.narrow;

    switch (static_cast<WseOpcode>(opcode)) {
    // Position vector operations (4100, 4102..4104 / 0x1004, 0x1006..0x1008).
    case WseOpcode::position_get_vector_to_position: {
        // 4100 (0x1004, 0x1401ecd5a): (dest_dist, pos_no, pos1, pos2).
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[1]) || !wse_register_ok(narrow[2]) ||
            !wse_register_ok(narrow[3]) || positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto dst_idx = static_cast<std::size_t>(narrow[1]);
        const auto p1_idx = static_cast<std::size_t>(narrow[2]);
        const auto p2_idx = static_cast<std::size_t>(narrow[3]);
        if (dst_idx >= positions.size() || p1_idx >= positions.size() ||
            p2_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        const WseVec4 origin = positions[p1_idx].o;
        const WseVec4 diff = positions[p2_idx].o - origin;
        WseTransform out = WseTransform::identity();
        out.o = origin;
        out.rotate_z(-std::atan2(diff.x, diff.y));
        const float horiz = std::sqrt(diff.x * diff.x + diff.y * diff.y);
        out.rotate_x(std::atan2(diff.z, horiz));
        positions[dst_idx] = out;
        effect.value = wse_round64(
            diff.length() *
            static_cast<float>(operands.state.fixed_point_multiplier));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::position_get_length: {
        // 4102 (0x1006, 0x1401ed0c1): (dest_len, pos_no) -> length(pos.o).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[1]) || positions.empty() ||
            static_cast<std::size_t>(narrow[1]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(
            positions[static_cast<std::size_t>(narrow[1])].o.length() *
            static_cast<float>(operands.state.fixed_point_multiplier));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::get_dot_product_of_positions: {
        // 4103 (0x1007, 0x1401ed10f): (dest, pos1, pos2) -> dot(p1.o, p2.o).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[1]) || !wse_register_ok(narrow[2]) ||
            positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto p1_idx = static_cast<std::size_t>(narrow[1]);
        const auto p2_idx = static_cast<std::size_t>(narrow[2]);
        if (p1_idx >= positions.size() || p2_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(
            positions[p1_idx].o.dot(positions[p2_idx].o) *
            static_cast<float>(operands.state.fixed_point_multiplier));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::get_cross_product_of_positions: {
        // 4104 (0x1008, 0x1401ed158): (dest_pos, pos1, pos2) -> cross -> dest.o.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || positions.empty()) {
            return WseDispatch::ignored;
        }
        const auto dst_idx = static_cast<std::size_t>(narrow[0]);
        const auto p1_idx = static_cast<std::size_t>(narrow[1]);
        const auto p2_idx = static_cast<std::size_t>(narrow[2]);
        if (dst_idx >= positions.size() || p1_idx >= positions.size() ||
            p2_idx >= positions.size()) {
            return WseDispatch::ignored;
        }
        positions[dst_idx].o = positions[p1_idx].o.cross(positions[p2_idx].o);
        return WseDispatch::handled_no_store;
    }

    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
