// Host tests for the portable executeWSE handlers.
//
// Build/run: powershell -File engine/build_host.ps1

#include <array>
#include <cfloat>
#include <cmath>
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
using wse::vm::WseEffect;
using wse::vm::WseOpcode;
using wse::vm::WseOperands;

constexpr std::uint32_t opcode(WseOpcode value) noexcept {
    return static_cast<std::uint32_t>(value);
}

WseDispatch run(std::uint32_t value,
                std::initializer_list<std::int64_t> values,
                std::initializer_list<std::int32_t> registers,
                WseEffect& effect,
                std::span<float> fp = {}) {
    WseOperands operands;
    operands.wide = std::span<const std::int64_t>(values.begin(), values.size());
    operands.narrow = std::span<const std::int32_t>(registers.begin(), registers.size());
    operands.fp = fp;
    effect.value = 0x1234;
    effect.store_value = true;
    return wse::vm::execute_wse(value, operands, effect);
}

void test_enum_values() {
    expect(opcode(WseOpcode::val_shr) == 2800, "val_shr opcode value");
    expect(opcode(WseOpcode::store_not) == 2809, "store_not opcode value");
    expect(opcode(WseOpcode::register_get) == 3000, "register_get opcode value");
    expect(opcode(WseOpcode::fld) == 4400, "fld opcode value");
    expect(opcode(WseOpcode::fadd) == 4438, "fadd opcode value");
    expect(opcode(WseOpcode::array_create) == 5000, "array_create opcode value");
    expect(opcode(WseOpcode::skeleton_model_clean_body_sections) == 5202,
           "final opcode value");
}

void test_shift_group() {
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    dispatch = run(opcode(WseOpcode::val_shr), {-8, 1}, {}, effect);
    expect(effect.value == -4 && dispatch == WseDispatch::handled &&
               effect.store_value,
           "val_shr is an arithmetic shift");

    dispatch = run(opcode(WseOpcode::val_shr), {-8, 65}, {}, effect);
    expect(effect.value == -4 && dispatch == WseDispatch::handled,
           "val_shr masks the shift count to 63");

    dispatch = run(opcode(WseOpcode::store_shr), {0, -8, 1}, {}, effect);
    expect(effect.value == -4 && dispatch == WseDispatch::handled,
           "store_shr reads the value from operand 1");

    dispatch = run(opcode(WseOpcode::val_lshr), {-8, 1}, {}, effect);
    expect(effect.value == static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFCULL) &&
               dispatch == WseDispatch::handled,
           "val_lshr is a logical shift");

    dispatch = run(opcode(WseOpcode::store_lshr), {0, -8, 1}, {}, effect);
    expect(effect.value == static_cast<std::int64_t>(0x7FFFFFFFFFFFFFFCULL) &&
               dispatch == WseDispatch::handled,
           "store_lshr is a logical shift");

    dispatch = run(opcode(WseOpcode::val_shl), {1, 63}, {}, effect);
    expect(static_cast<std::uint64_t>(effect.value) == 0x8000000000000000ULL &&
               dispatch == WseDispatch::handled,
           "val_shl shifts into the sign bit");

    dispatch = run(opcode(WseOpcode::store_shl), {0, 1, 63}, {}, effect);
    expect(static_cast<std::uint64_t>(effect.value) == 0x8000000000000000ULL &&
               dispatch == WseDispatch::handled,
           "store_shl shifts into the sign bit");
}

void test_bitwise_group() {
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    dispatch = run(opcode(WseOpcode::val_xor), {0x0F0F, 0x00FF}, {}, effect);
    expect(effect.value == 0x0FF0 && dispatch == WseDispatch::handled,
           "val_xor combines operands 0 and 1");

    dispatch = run(opcode(WseOpcode::store_xor), {0, 0x0F0F, 0x00FF}, {}, effect);
    expect(effect.value == 0x0FF0 && dispatch == WseDispatch::handled,
           "store_xor combines operands 1 and 2");

    dispatch = run(opcode(WseOpcode::val_not), {0}, {}, effect);
    expect(effect.value == -1 && dispatch == WseDispatch::handled,
           "val_not complements operand 0");

    dispatch = run(opcode(WseOpcode::store_not), {0, 0}, {}, effect);
    expect(effect.value == -1 && dispatch == WseDispatch::handled,
           "store_not complements operand 1");
}

void test_operand_counts() {
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    dispatch = run(opcode(WseOpcode::val_shr), {-8}, {}, effect);
    expect(effect.value == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "val_shr rejects a missing shift operand");

    dispatch = run(opcode(WseOpcode::store_shr), {0, -8}, {}, effect);
    expect(effect.value == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "store_shr rejects a missing shift operand");

    dispatch = run(opcode(WseOpcode::val_not), {}, {}, effect);
    expect(effect.value == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "val_not rejects a missing value operand");

    dispatch = run(opcode(WseOpcode::store_not), {0}, {}, effect);
    expect(effect.value == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "store_not rejects a missing value operand");
}

void test_unimplemented_and_mask() {
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    dispatch = run(opcode(WseOpcode::register_get), {1}, {}, effect);
    expect(effect.value == 0x1234 && dispatch == WseDispatch::unimplemented,
           "unimplemented opcodes leave the result untouched");

    dispatch = run(0x40000000U | opcode(WseOpcode::val_shr), {-8, 1}, {}, effect);
    expect(effect.value == -4 && dispatch == WseDispatch::handled,
           "executeWSE masks opcode flags");

    dispatch = run(0x10000000U | opcode(WseOpcode::val_shr), {-8, 1}, {}, effect);
    expect(effect.value == -4 && dispatch == WseDispatch::handled,
           "executeWSE clears the high opcode bits");
}

void test_fp_load_store() {
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    dispatch = run(opcode(WseOpcode::fld), {0, 150}, {1}, effect, fp);
    expect(dispatch == WseDispatch::handled_no_store && !effect.store_value &&
               fp[1] == 1.5f,
           "fld divides the fixed-point value by 100");

    dispatch = run(opcode(WseOpcode::fld), {0, -50}, {2}, effect, fp);
    expect(dispatch == WseDispatch::handled_no_store && fp[2] == -0.5f,
           "fld keeps the sign of negative fixed-point values");

    fp[3] = 1.25f;
    dispatch = run(opcode(WseOpcode::fst), {0, 0}, {0, 3}, effect, fp);
    expect(dispatch == WseDispatch::handled && effect.store_value &&
               effect.value == 125,
           "fst multiplies by 100 and rounds through operand 0");

    fp[4] = 1.236f;
    dispatch = run(opcode(WseOpcode::fst), {0, 0}, {0, 4}, effect, fp);
    expect(dispatch == WseDispatch::handled && effect.value == 124,
           "fst rounds a fraction above one half up");

    fp[5] = 7.5f;
    dispatch = run(opcode(WseOpcode::fcpy), {}, {6, 5}, effect, fp);
    expect(dispatch == WseDispatch::handled_no_store && fp[6] == 7.5f,
           "fcpy copies the source register");
}

void test_fp_compare() {
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    WseEffect effect;

    fp[0] = 1.5f;
    fp[1] = 1.5f;
    expect(run(opcode(WseOpcode::feq), {}, {0, 1}, effect, fp) ==
               WseDispatch::handled_no_store,
           "feq accepts equal registers");

    fp[1] = std::nextafter(1.5f, 2.0f);
    expect(run(opcode(WseOpcode::feq), {}, {0, 1}, effect, fp) ==
               WseDispatch::handled_no_store,
           "feq accepts a difference within FLT_EPSILON");

    fp[1] = 1.75f;
    expect(run(opcode(WseOpcode::feq), {}, {0, 1}, effect, fp) ==
               WseDispatch::condition_false,
           "feq rejects a larger difference");

    fp[0] = 2.0f;
    fp[1] = 1.0f;
    expect(run(opcode(WseOpcode::fgt), {}, {0, 1}, effect, fp) ==
               WseDispatch::handled_no_store,
           "fgt passes when operand 0 is greater");
    expect(run(opcode(WseOpcode::flt), {}, {0, 1}, effect, fp) ==
               WseDispatch::condition_false,
           "flt fails when operand 0 is greater");
    expect(run(opcode(WseOpcode::fge), {}, {0, 1}, effect, fp) ==
               WseDispatch::handled_no_store,
           "fge passes when operand 0 is greater");
    expect(run(opcode(WseOpcode::fle), {}, {0, 1}, effect, fp) ==
               WseDispatch::condition_false,
           "fle fails when operand 0 is greater");

    fp[0] = 1.0f;
    fp[1] = 1.0f;
    expect(run(opcode(WseOpcode::fgt), {}, {0, 1}, effect, fp) ==
               WseDispatch::condition_false,
           "fgt rejects equal registers");
    expect(run(opcode(WseOpcode::fge), {}, {0, 1}, effect, fp) ==
               WseDispatch::handled_no_store,
           "fge accepts equal registers");
    expect(run(opcode(WseOpcode::fle), {}, {0, 1}, effect, fp) ==
               WseDispatch::handled_no_store,
           "fle accepts equal registers");
}

void test_fp_arithmetic() {
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    WseEffect effect;

    fp[1] = 2.5f;
    fp[2] = 1.25f;

    expect(run(opcode(WseOpcode::fsub), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 1.25f,
           "fsub subtracts operand 2 from operand 1");
    expect(run(opcode(WseOpcode::fmul), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 3.125f,
           "fmul multiplies operands 1 and 2");
    expect(run(opcode(WseOpcode::fdiv), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 2.0f,
           "fdiv divides operand 1 by operand 2");
    expect(run(opcode(WseOpcode::fadd), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 3.75f,
           "fadd adds operands 1 and 2");

    fp[0] = 99.0f;
    fp[2] = 0.0f;
    expect(run(opcode(WseOpcode::fdiv), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 99.0f,
           "fdiv by zero leaves the destination unchanged");

    fp[0] = 0.0f;
    fp[1] = 2.5f;
    fp[2] = 1.25f;
    expect(run(opcode(WseOpcode::fmin), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 1.25f,
           "fmin selects the smaller register");
    expect(run(opcode(WseOpcode::fmax), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 2.5f,
           "fmax selects the larger register");

    fp[1] = 0.0f;
    fp[2] = 1.0f;
    fp[3] = 2.0f;
    expect(run(opcode(WseOpcode::fclamp), {}, {0, 1, 2, 3}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 1.0f,
           "fclamp raises values below the lower bound");
    fp[1] = 1.5f;
    expect(run(opcode(WseOpcode::fclamp), {}, {0, 1, 2, 3}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 1.5f,
           "fclamp keeps values inside the bounds");
    fp[1] = 5.0f;
    expect(run(opcode(WseOpcode::fclamp), {}, {0, 1, 2, 3}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 2.0f,
           "fclamp lowers values above the upper bound");
    fp[1] = 5.0f;
    expect(run(opcode(WseOpcode::fclamp), {}, {0, 1, 3, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 2.0f,
           "fclamp accepts the bounds in either order");
}

void test_fp_errors() {
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    fp[0] = 42.0f;
    dispatch = run(opcode(WseOpcode::fld), {0, 150}, {128}, effect, fp);
    expect(dispatch == WseDispatch::ignored && fp[0] == 42.0f,
           "fld rejects register 128 (mbCheckRegister accepts 0..127)");

    dispatch = run(opcode(WseOpcode::fld), {0, 150}, {-1}, effect, fp);
    expect(dispatch == WseDispatch::ignored,
           "fld rejects a negative register index");

    dispatch = run(opcode(WseOpcode::fld), {0, 150}, {1}, effect);
    expect(dispatch == WseDispatch::ignored,
           "fld without a register bank is a no-op");

    dispatch = run(opcode(WseOpcode::fld), {0}, {}, effect, fp);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "fld rejects a missing value operand");

    dispatch = run(opcode(WseOpcode::fsub), {}, {0, 1}, effect, fp);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "fsub rejects a missing operand");
}

void test_descriptions() {
    using wse::vm::describe;
    expect(!describe(WseDispatch::handled).empty(), "handled description");
    expect(!describe(WseDispatch::handled_no_store).empty(),
           "no-store description");
    expect(!describe(WseDispatch::condition_false).empty(),
           "condition description");
    expect(!describe(WseDispatch::ignored).empty(), "ignored description");
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
    test_fp_load_store();
    test_fp_compare();
    test_fp_arithmetic();
    test_fp_errors();
    test_descriptions();

    if (failures != 0) {
        std::printf("wse tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("wse tests: PASS\n");
    return 0;
}
