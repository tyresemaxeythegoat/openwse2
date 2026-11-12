// Host tests for the first portable executeWSE handler group.
//
// Build/run: powershell -File engine/build_host.ps1

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>

#include "wse/vm/execute_wse.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

using wse::vm::WseDispatch;
using wse::vm::WseOpcode;
using wse::vm::WseOperands;

constexpr std::uint32_t opcode(WseOpcode value) noexcept {
    return static_cast<std::uint32_t>(value);
}

std::int64_t run(std::uint32_t value,
                 std::initializer_list<std::int64_t> values,
                 WseDispatch& dispatch) {
    WseOperands operands;
    operands.wide = std::span<const std::int64_t>(values.begin(), values.size());
    std::int64_t result = 0x1234;
    dispatch = wse::vm::execute_wse(value, operands, result);
    return result;
}

void test_enum_values() {
    expect(opcode(WseOpcode::val_shr) == 2800, "val_shr opcode value");
    expect(opcode(WseOpcode::store_not) == 2809, "store_not opcode value");
    expect(opcode(WseOpcode::register_get) == 3000, "register_get opcode value");
    expect(opcode(WseOpcode::array_create) == 5000, "array_create opcode value");
    expect(opcode(WseOpcode::skeleton_model_clean_body_sections) == 5202,
           "final opcode value");
}

void test_shift_group() {
    WseDispatch dispatch = WseDispatch::unimplemented;

    expect(run(opcode(WseOpcode::val_shr), {-8, 1}, dispatch) == -4 &&
               dispatch == WseDispatch::handled,
           "val_shr is an arithmetic shift");
    expect(run(opcode(WseOpcode::val_shr), {-8, 65}, dispatch) == -4 &&
               dispatch == WseDispatch::handled,
           "val_shr masks the shift count to 63");
    expect(run(opcode(WseOpcode::store_shr), {0, -8, 1}, dispatch) == -4 &&
               dispatch == WseDispatch::handled,
           "store_shr reads the value from operand 1");
    expect(run(opcode(WseOpcode::val_lshr), {-8, 1}, dispatch) ==
               static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFCULL) &&
               dispatch == WseDispatch::handled,
           "val_lshr is a logical shift");
    expect(run(opcode(WseOpcode::store_lshr), {0, -8, 1}, dispatch) ==
               static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFCULL) &&
               dispatch == WseDispatch::handled,
           "store_lshr is a logical shift");
    expect(static_cast<std::uint64_t>(
               run(opcode(WseOpcode::val_shl), {1, 63}, dispatch)) ==
               0x8000000000000000ULL &&
               dispatch == WseDispatch::handled,
           "val_shl shifts into the sign bit");
    expect(static_cast<std::uint64_t>(
               run(opcode(WseOpcode::store_shl), {0, 1, 63}, dispatch)) ==
               0x8000000000000000ULL &&
               dispatch == WseDispatch::handled,
           "store_shl shifts into the sign bit");
}

void test_bitwise_group() {
    WseDispatch dispatch = WseDispatch::unimplemented;

    expect(run(opcode(WseOpcode::val_xor), {0x0F0F, 0x00FF}, dispatch) == 0x0FF0 &&
               dispatch == WseDispatch::handled,
           "val_xor combines operands 0 and 1");
    expect(run(opcode(WseOpcode::store_xor), {0, 0x0F0F, 0x00FF}, dispatch) ==
               0x0FF0 &&
               dispatch == WseDispatch::handled,
           "store_xor combines operands 1 and 2");
    expect(run(opcode(WseOpcode::val_not), {0}, dispatch) == -1 &&
               dispatch == WseDispatch::handled,
           "val_not complements operand 0");
    expect(run(opcode(WseOpcode::store_not), {0, 0}, dispatch) == -1 &&
               dispatch == WseDispatch::handled,
           "store_not complements operand 1");
}

void test_operand_counts() {
    WseDispatch dispatch = WseDispatch::unimplemented;

    expect(run(opcode(WseOpcode::val_shr), {-8}, dispatch) == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "val_shr rejects a missing shift operand");
    expect(run(opcode(WseOpcode::store_shr), {0, -8}, dispatch) == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "store_shr rejects a missing shift operand");
    expect(run(opcode(WseOpcode::val_not), {}, dispatch) == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "val_not rejects a missing value operand");
    expect(run(opcode(WseOpcode::store_not), {0}, dispatch) == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "store_not rejects a missing value operand");
}

void test_unimplemented_and_mask() {
    WseDispatch dispatch = WseDispatch::unimplemented;

    expect(run(opcode(WseOpcode::register_get), {1}, dispatch) == 0x1234 &&
               dispatch == WseDispatch::unimplemented,
           "unimplemented opcodes leave the result untouched");
    expect(run(0x40000000U | opcode(WseOpcode::val_shr), {-8, 1}, dispatch) == -4 &&
               dispatch == WseDispatch::handled,
           "executeWSE masks opcode flags");
    expect(run(0x10000000U | opcode(WseOpcode::val_shr), {-8, 1}, dispatch) == -4 &&
               dispatch == WseDispatch::handled,
           "executeWSE clears the high opcode bits");
}

void test_descriptions() {
    using wse::vm::describe;
    expect(!describe(WseDispatch::handled).empty(), "handled description");
    expect(!describe(WseDispatch::unimplemented).empty(), "unimplemented description");
    expect(!describe(WseDispatch::operand_count_out_of_range).empty(),
           "operand count description");
}

}  // namespace

int main() {
    test_enum_values();
    test_shift_group();
    test_bitwise_group();
    test_operand_counts();
    test_unimplemented_and_mask();
    test_descriptions();

    if (failures != 0) {
        std::printf("wse tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("wse tests: PASS\n");
    return 0;
}
