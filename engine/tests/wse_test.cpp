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

bool close(float lhs, float rhs, float tolerance = 1e-5f) {
    return std::fabs(lhs - rhs) <= tolerance;
}

using wse::vm::WseDispatch;
using wse::vm::WseEffect;
using wse::vm::WseOpcode;
using wse::vm::WseOperands;
using wse::vm::WseVmState;

constexpr std::uint32_t opcode(WseOpcode value) noexcept {
    return static_cast<std::uint32_t>(value);
}

WseDispatch run(std::uint32_t value,
                std::initializer_list<std::int64_t> values,
                std::initializer_list<std::int32_t> registers,
                WseEffect& effect,
                std::span<float> fp = {},
                std::span<std::int64_t> bank = {},
                WseVmState state = {},
                std::initializer_list<std::uint8_t> tags = {}) {
    WseOperands operands;
    operands.wide = std::span<const std::int64_t>(values.begin(), values.size());
    operands.narrow = std::span<const std::int32_t>(registers.begin(), registers.size());
    operands.tags = std::span<const std::uint8_t>(tags.begin(), tags.size());
    operands.fp = fp;
    operands.registers = bank;
    operands.state = state;
    effect.value = 0x1234;
    effect.store_value = true;
    return wse::vm::execute_wse(value, operands, effect);
}

void test_enum_values() {
    expect(opcode(WseOpcode::val_shr) == 2800, "val_shr opcode value");
    expect(opcode(WseOpcode::store_not) == 2809, "store_not opcode value");
    expect(opcode(WseOpcode::register_get) == 3000, "register_get opcode value");
    expect(opcode(WseOpcode::fld) == 4400, "fld opcode value");
    expect(opcode(WseOpcode::fsqrt) == 4418, "fsqrt opcode value");
    expect(opcode(WseOpcode::fatan2) == 4436, "fatan2 opcode value");
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

    dispatch = run(opcode(WseOpcode::array_create), {1}, {}, effect);
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

void test_fp_math() {
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    WseEffect effect;

    fp[1] = 9.0f;
    expect(run(opcode(WseOpcode::fsqrt), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 3.0f),
           "fsqrt takes the square root");

    fp[1] = -2.5f;
    expect(run(opcode(WseOpcode::fabs), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 2.5f),
           "fabs clears the sign");

    fp[1] = 1.2f;
    expect(run(opcode(WseOpcode::fceil), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 2.0f),
           "fceil rounds up");

    fp[1] = -1.2f;
    expect(run(opcode(WseOpcode::ffloor), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], -2.0f),
           "ffloor rounds down");

    fp[1] = 0.0f;
    expect(run(opcode(WseOpcode::fexp), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 1.0f),
           "fexp(0) is one");

    fp[1] = 2.0f;
    fp[2] = 10.0f;
    expect(run(opcode(WseOpcode::fpow), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 1024.0f),
           "fpow raises operand 1 to operand 2");

    fp[1] = static_cast<float>(std::exp(1.0));
    expect(run(opcode(WseOpcode::fln), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 1.0f),
           "fln is the natural logarithm");

    fp[1] = 1000.0f;
    expect(run(opcode(WseOpcode::flog), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 3.0f),
           "flog defaults to base 10");

    fp[0] = 42.0f;
    fp[1] = 512.0f;
    expect(run(opcode(WseOpcode::flog), {}, {0, 1, 8}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 3.0f),
           "flog divides log10(value) by log10(base)");

    fp[0] = 42.0f;
    expect(run(opcode(WseOpcode::flog), {}, {0, 1, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 42.0f,
           "flog rejects a base below two without storing");

    fp[1] = 7.5f;
    fp[2] = 2.0f;
    expect(run(opcode(WseOpcode::fmod), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 1.5f),
           "fmod divides operand 1 by operand 2");

    fp[0] = 42.0f;
    fp[2] = 0.0f;
    expect(run(opcode(WseOpcode::fmod), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               fp[0] == 42.0f,
           "fmod by zero leaves the destination unchanged");

    fp[0] = 0.0f;
    fp[1] = 0.5f;
    expect(run(opcode(WseOpcode::fsin), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::sin(0.5f)),
           "fsin calls sin");
    expect(run(opcode(WseOpcode::fcos), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::cos(0.5f)),
           "fcos calls cos");
    expect(run(opcode(WseOpcode::ftan), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::tan(0.5f)),
           "ftan calls tan");
    expect(run(opcode(WseOpcode::fsinh), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::sinh(0.5f)),
           "fsinh calls sinh");
    expect(run(opcode(WseOpcode::fcosh), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::cosh(0.5f)),
           "fcosh calls cosh");
    expect(run(opcode(WseOpcode::ftanh), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::tanh(0.5f)),
           "ftanh calls tanh");

    fp[1] = 0.5f;
    expect(run(opcode(WseOpcode::fasin), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::asin(0.5f)),
           "fasin calls asin");
    expect(run(opcode(WseOpcode::facos), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::acos(0.5f)),
           "facos calls acos");
    expect(run(opcode(WseOpcode::fatan), {}, {0, 1}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], std::atan(0.5f)),
           "fatan calls atan");

    fp[1] = 1.0f;
    fp[2] = 1.0f;
    expect(run(opcode(WseOpcode::fatan2), {}, {0, 1, 2}, effect, fp) ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 0.78539816f),
           "fatan2 passes operand 2 as x");

    fp[0] = 42.0f;
    expect(run(opcode(WseOpcode::fsqrt), {}, {0, 128}, effect, fp) ==
                   WseDispatch::ignored &&
               fp[0] == 42.0f,
           "fsqrt rejects register 128");

    expect(run(opcode(WseOpcode::fpow), {}, {0, 1}, effect, fp) ==
                   WseDispatch::operand_count_out_of_range,
           "fpow rejects a missing exponent register");
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

void test_register_group() {
    std::array<std::int64_t, wse::vm::kFpRegisterCount> bank{};
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;

    bank[5] = 77;
    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 5}, effect, {}, bank);
    expect(dispatch == WseDispatch::handled && effect.store_value &&
               effect.value == 77,
           "register_get reads the int64 register bank");

    dispatch = run(opcode(WseOpcode::register_set), {0, 99}, {3}, effect, {}, bank);
    expect(dispatch == WseDispatch::handled_no_store && !effect.store_value &&
               bank[3] == 99,
           "register_set writes operand 1 into the named register");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 3}, effect, {}, bank);
    expect(dispatch == WseDispatch::handled && effect.value == 99,
           "register_set and register_get share the same bank");

    dispatch = run(opcode(WseOpcode::store_wse_version), {0}, {0, 0}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 5,
           "store_wse_version 0 reports WSE version 5");

    dispatch = run(opcode(WseOpcode::store_wse_version), {0}, {0, 1}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 0x483,
           "store_wse_version 1 reports Warband build 1155");

    dispatch = run(opcode(WseOpcode::store_wse_version), {0}, {0, 2}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "store_wse_version defaults an unknown argument to 0");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 128}, effect, {}, bank);
    expect(dispatch == WseDispatch::ignored,
           "register_get rejects register 128 (mbCheckRegister accepts 0..127)");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, -1}, effect, {}, bank);
    expect(dispatch == WseDispatch::ignored,
           "register_get rejects a negative register index");

    dispatch = run(opcode(WseOpcode::register_get), {0, 0}, {0, 5}, effect);
    expect(dispatch == WseDispatch::ignored,
           "register_get without a register bank is a no-op");

    dispatch = run(opcode(WseOpcode::register_get), {0}, {0}, effect, {}, bank);
    expect(effect.value == 0x1234 &&
               dispatch == WseDispatch::operand_count_out_of_range,
           "register_get rejects a missing register index");

    dispatch = run(opcode(WseOpcode::register_set), {0}, {}, effect, {}, bank);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "register_set rejects a missing destination register");

    dispatch = run(opcode(WseOpcode::store_wse_version), {}, {0}, effect);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "store_wse_version rejects a missing selector");
}

void test_script_context_group() {
    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;
    WseVmState state;

    dispatch = run(opcode(WseOpcode::store_current_trigger), {}, {}, effect);
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "store_current_trigger is 0 outside a trigger");

    state.caller = -3;
    dispatch = run(opcode(WseOpcode::store_current_trigger), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == -3,
           "store_current_trigger returns the negative trigger context");

    state.caller = 7;
    dispatch = run(opcode(WseOpcode::store_current_trigger), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "store_current_trigger ignores a positive context");

    state = {};
    state.num_return_values = 3;
    dispatch = run(opcode(WseOpcode::store_num_return_values), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 3,
           "store_num_return_values reads the count");

    std::array<std::int64_t, 16> returns{11, 22, 33};
    state.return_values = returns;
    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 2}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::handled && effect.value == 22,
           "store_return_value reads the 1-based slot");

    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 4}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::ignored,
           "store_return_value rejects an unset slot");

    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 0}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::ignored,
           "store_return_value rejects index 0");

    dispatch = run(opcode(WseOpcode::store_return_value), {0, 0}, {0, 17}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::ignored,
           "store_return_value rejects index 17");

    dispatch = run(opcode(WseOpcode::store_return_value), {0}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "store_return_value rejects a missing index");

    state = {};
    state.app_time_seconds = 1.5;
    dispatch = run(opcode(WseOpcode::store_application_time), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 1500,
           "store_application_time converts seconds to milliseconds");

    state.app_time_seconds = 2.25;
    dispatch = run(opcode(WseOpcode::store_application_time), {}, {}, effect, {}, {},
                   state);
    expect(dispatch == WseDispatch::handled && effect.value == 2250,
           "store_application_time rounds to the nearest millisecond");

    state = {};
    dispatch = run(opcode(WseOpcode::store_cur_mission_template_no), {}, {}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "store_cur_mission_template_no is -1 without a mission");

    std::int32_t template_no = 42;
    state.current_mission_template_no = &template_no;
    dispatch = run(opcode(WseOpcode::store_cur_mission_template_no), {}, {}, effect, {},
                   {}, state);
    expect(dispatch == WseDispatch::handled && effect.value == 42,
           "store_cur_mission_template_no reads the active template");
}

void test_string_group() {
    std::u16string registers[128] = {
        u"alpha",  // 0
        u"ALPHA",  // 1
        u"beta",   // 2
        u"world",  // 3
        u"hello",  // 4
        u"a b",    // 5
        u"123",    // 6
        u"12a",    // 7
        u" \t",    // 8
        u"abcabc", // 9
        u"bc",     // 10
        u"12.5",   // 11
        u"12abc",  // 12
        u" 42",    // 13
        u"abc",    // 14
        u"",       // 15
        u"HELLO",  // 16
        u"a1",     // 17
        u"1a",     // 18
        u"0.29",   // 19
        u"-7.9",   // 20
        u"1e3",    // 21
        u"-0.29",  // 22
        u" ",      // 23
        u"WORLD",  // 24
    };
    const std::u16string_view global[] = {u"global0", u"hello world"};
    const std::u16string_view local[] = {u"local0", u"xyz"};

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(registers);
    state.strings.global = std::span<const std::u16string_view>(global);
    state.strings.local = std::span<const std::u16string_view>(local);

    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;
    const auto call = [&](WseOpcode op, std::initializer_list<std::int32_t> operands,
                          std::initializer_list<std::uint8_t> tags) {
        return run(opcode(op), {}, operands, effect, {}, {}, state, tags);
    };

    // Tag resolution and mbCheckStringRegister bounds.
    dispatch = call(WseOpcode::str_length, {0, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 5,
           "str_length reads a literal by tag 0");
    dispatch = call(WseOpcode::str_length, {0, 0}, {0, 3});
    expect(dispatch == WseDispatch::handled && effect.value == 7,
           "str_length reads a global register by tag 3");
    dispatch = call(WseOpcode::str_length, {0, 0}, {0, 0x16});
    expect(dispatch == WseDispatch::handled && effect.value == 6,
           "str_length reads a local register by tag 0x16");
    dispatch = call(WseOpcode::str_length, {0, 0}, {0, 9});
    expect(dispatch == WseDispatch::handled && effect.value == 7,
           "an unknown string tag resolves through the global bank");
    dispatch = call(WseOpcode::str_length, {0, 127}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "literal index 127 passes the check but resolves empty");
    dispatch = call(WseOpcode::str_length, {0, 128}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "literal index 128 fails mbCheckStringRegister");
    dispatch = call(WseOpcode::str_length, {0, 2}, {0, 3});
    expect(dispatch == WseDispatch::ignored,
           "a global index at the count fails mbCheckStringRegister");
    dispatch = call(WseOpcode::str_length, {0, 2}, {0, 0x16});
    expect(dispatch == WseDispatch::ignored,
           "a local index at the count fails mbCheckStringRegister");
    dispatch = call(WseOpcode::str_length, {0, 99}, {0, 9});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "an unknown tag out of range resolves empty instead of failing");

    // str_equals.
    dispatch = call(WseOpcode::str_equals, {0, 1}, {0, 0});
    expect(dispatch == WseDispatch::condition_false,
           "str_equals is case-sensitive by default");
    dispatch = call(WseOpcode::str_equals, {0, 1, 1}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_equals folds case when operand 2 is nonzero");
    dispatch = call(WseOpcode::str_equals, {0, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_equals accepts identical strings");
    dispatch = call(WseOpcode::str_equals, {0, 0}, {3, 0x16});
    expect(dispatch == WseDispatch::condition_false,
           "str_equals compares the global and local banks");
    dispatch = call(WseOpcode::str_equals, {0, 0}, {7, 3});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_equals resolves an unknown tag through the global bank");
    dispatch = call(WseOpcode::str_equals, {128, 0}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_equals ignores an invalid string operand");
    dispatch = call(WseOpcode::str_equals, {0}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_equals rejects a missing second operand");

    // str_contains.
    dispatch = call(WseOpcode::str_contains, {1, 3}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_contains finds a substring");
    dispatch = call(WseOpcode::str_contains, {1, 4}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_contains finds a leading substring");
    dispatch = call(WseOpcode::str_contains, {1, 1}, {3, 0x16});
    expect(dispatch == WseDispatch::condition_false,
           "str_contains rejects a missing substring");
    dispatch = call(WseOpcode::str_contains, {1, 15}, {3, 0});
    expect(dispatch == WseDispatch::condition_false,
           "str_contains treats an empty needle as not contained");
    dispatch = call(WseOpcode::str_contains, {1, 16}, {3, 0});
    expect(dispatch == WseDispatch::condition_false,
           "str_contains is case-sensitive by default");
    dispatch = call(WseOpcode::str_contains, {1, 16, 1}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_contains folds case when operand 2 is nonzero");
    dispatch = call(WseOpcode::str_contains, {128, 0}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_contains ignores an invalid string operand");
    dispatch = call(WseOpcode::str_contains, {1}, {3});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_contains rejects a missing second operand");

    // str_starts_with / str_ends_with.
    dispatch = call(WseOpcode::str_starts_with, {1, 4}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_starts_with matches a prefix");
    dispatch = call(WseOpcode::str_starts_with, {1, 3}, {3, 0});
    expect(dispatch == WseDispatch::condition_false,
           "str_starts_with rejects a non-prefix");
    dispatch = call(WseOpcode::str_starts_with, {1, 16}, {3, 0});
    expect(dispatch == WseDispatch::condition_false,
           "str_starts_with is case-sensitive by default");
    dispatch = call(WseOpcode::str_starts_with, {1, 16, 1}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_starts_with folds case when operand 2 is nonzero");
    dispatch = call(WseOpcode::str_starts_with, {1, 15}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_starts_with accepts an empty prefix");
    dispatch = call(WseOpcode::str_starts_with, {128, 0}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_starts_with ignores an invalid string operand");

    dispatch = call(WseOpcode::str_ends_with, {1, 3}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_ends_with matches a suffix");
    dispatch = call(WseOpcode::str_ends_with, {1, 4}, {3, 0});
    expect(dispatch == WseDispatch::condition_false,
           "str_ends_with rejects a non-suffix");
    dispatch = call(WseOpcode::str_ends_with, {1, 24}, {3, 0});
    expect(dispatch == WseDispatch::condition_false,
           "str_ends_with is case-sensitive by default");
    dispatch = call(WseOpcode::str_ends_with, {1, 24, 1}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_ends_with folds case when operand 2 is nonzero");
    dispatch = call(WseOpcode::str_ends_with, {1, 15}, {3, 0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_ends_with accepts an empty suffix");
    dispatch = call(WseOpcode::str_ends_with, {128, 0}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_ends_with ignores an invalid string operand");

    // str_is_alpha.
    dispatch = call(WseOpcode::str_is_alpha, {0}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_alpha scans the whole string without an index");
    dispatch = call(WseOpcode::str_is_alpha, {17}, {0});
    expect(dispatch == WseDispatch::condition_false,
           "str_is_alpha rejects a string with a digit");
    dispatch = call(WseOpcode::str_is_alpha, {0, 0}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_alpha checks a single index");
    dispatch = call(WseOpcode::str_is_alpha, {18, 0}, {0});
    expect(dispatch == WseDispatch::condition_false,
           "str_is_alpha rejects a digit at the index");
    dispatch = call(WseOpcode::str_is_alpha, {18, 1}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_alpha accepts a letter at the index");
    dispatch = call(WseOpcode::str_is_alpha, {0, 5}, {0});
    expect(dispatch == WseDispatch::ignored,
           "str_is_alpha ignores an index at the string length");
    dispatch = call(WseOpcode::str_is_alpha, {0, -1}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_alpha treats index -1 as the whole string");
    dispatch = call(WseOpcode::str_is_alpha, {0, -2}, {0});
    expect(dispatch == WseDispatch::ignored,
           "str_is_alpha ignores another negative index");
    dispatch = call(WseOpcode::str_is_alpha, {1}, {3});
    expect(dispatch == WseDispatch::condition_false,
           "str_is_alpha rejects the space in a global string");
    dispatch = call(WseOpcode::str_is_alpha, {15}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_alpha accepts the empty string");

    // str_is_digit.
    dispatch = call(WseOpcode::str_is_digit, {6}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_digit scans the whole numeric string");
    dispatch = call(WseOpcode::str_is_digit, {7}, {0});
    expect(dispatch == WseDispatch::condition_false,
           "str_is_digit rejects a string with a letter");
    dispatch = call(WseOpcode::str_is_digit, {7, 2}, {0});
    expect(dispatch == WseDispatch::condition_false,
           "str_is_digit rejects a letter at the index");
    dispatch = call(WseOpcode::str_is_digit, {7, 0}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_digit accepts a digit at the index");
    dispatch = call(WseOpcode::str_is_digit, {0, 5}, {0x16});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_digit reads a local string register");
    dispatch = call(WseOpcode::str_is_digit, {0, 6}, {0x16});
    expect(dispatch == WseDispatch::ignored,
           "str_is_digit ignores an index at the string length");

    // str_is_whitespace.
    dispatch = call(WseOpcode::str_is_whitespace, {8}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_whitespace accepts spaces and tabs");
    dispatch = call(WseOpcode::str_is_whitespace, {5}, {0});
    expect(dispatch == WseDispatch::condition_false,
           "str_is_whitespace rejects letters");
    dispatch = call(WseOpcode::str_is_whitespace, {5, 1}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_whitespace checks a single space");
    dispatch = call(WseOpcode::str_is_whitespace, {23}, {0});
    expect(dispatch == WseDispatch::handled_no_store,
           "str_is_whitespace accepts a single space");
    dispatch = call(WseOpcode::str_is_whitespace, {0}, {0});
    expect(dispatch == WseDispatch::condition_false,
           "str_is_whitespace rejects a word");
    dispatch = call(WseOpcode::str_is_whitespace, {5, 3}, {0});
    expect(dispatch == WseDispatch::ignored,
           "str_is_whitespace ignores an index at the string length");

    // str_length.
    dispatch = call(WseOpcode::str_length, {0, 1}, {0, 3});
    expect(dispatch == WseDispatch::handled && effect.value == 11,
           "str_length measures a global string");
    dispatch = call(WseOpcode::str_length, {0, 15}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "str_length of the empty string is zero");
    dispatch = call(WseOpcode::str_length, {0, 1}, {0, 0x16});
    expect(dispatch == WseDispatch::handled && effect.value == 3,
           "str_length measures a local string");
    dispatch = call(WseOpcode::str_length, {0, 128}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_length ignores an invalid string operand");
    dispatch = call(WseOpcode::str_length, {0}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_length rejects a missing source operand");

    // str_index_of.
    dispatch = call(WseOpcode::str_index_of, {0, 9, 10}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1,
           "str_index_of finds the first occurrence");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 10, 2}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 4,
           "str_index_of honours the start operand");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 10, 0, 3}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1,
           "str_index_of accepts a match before the end operand");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 10, 2, 3}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_index_of rejects a match after the end operand");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 10, 6}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_index_of rejects a start at the string length");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 10, 0, 6}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_index_of rejects an end at the string length");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 10, 4, 2}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_index_of rejects an end before the start");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 0}, {0, 0, 0x16});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_index_of reports -1 for a missing needle");
    dispatch = call(WseOpcode::str_index_of, {0, 9, 15}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_index_of reports -1 for an empty needle");
    dispatch = call(WseOpcode::str_index_of, {0, 128, 10}, {0, 0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_index_of ignores an invalid haystack");
    dispatch = call(WseOpcode::str_index_of, {0, 9}, {0, 0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_index_of rejects a missing needle operand");

    // str_last_index_of.
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 10}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 4,
           "str_last_index_of finds the last occurrence");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 10, 0, 2}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1,
           "str_last_index_of returns the previous match before the end");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 10, 0, 3}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_last_index_of mirrors the retail loop when the next match "
           "passes the end");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 10, 2}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 4,
           "str_last_index_of honours the start operand");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 10, 4, 5}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 4,
           "str_last_index_of accepts a match at the end");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 10, 5, 5}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_last_index_of rejects a start at the end");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 10, 0, 0}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_last_index_of rejects an end at the start");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 0}, {0, 0, 0x16});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_last_index_of reports -1 for a missing needle");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9, 15}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_last_index_of reports -1 for an empty needle");
    dispatch = call(WseOpcode::str_last_index_of, {0, 128, 10}, {0, 0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_last_index_of ignores an invalid haystack");
    dispatch = call(WseOpcode::str_last_index_of, {0, 9}, {0, 0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_last_index_of rejects a missing needle operand");

    // str_get_char.
    dispatch = call(WseOpcode::str_get_char, {0, 0, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == u'a',
           "str_get_char reads the first literal character");
    dispatch = call(WseOpcode::str_get_char, {0, 0, 4}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == u'a',
           "str_get_char reads the last literal character");
    dispatch = call(WseOpcode::str_get_char, {0, 1, 6}, {0, 3});
    expect(dispatch == WseDispatch::handled && effect.value == u'w',
           "str_get_char reads a global string character");
    dispatch = call(WseOpcode::str_get_char, {0, 0, 1}, {0, 0x16});
    expect(dispatch == WseDispatch::handled && effect.value == u'o',
           "str_get_char reads a local string character");
    dispatch = call(WseOpcode::str_get_char, {0, 0, 5}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_get_char ignores an index at the string length");
    dispatch = call(WseOpcode::str_get_char, {0, 0, -1}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_get_char ignores a negative index");
    dispatch = call(WseOpcode::str_get_char, {0, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == u'a',
           "str_get_char defaults a missing index to zero");
    dispatch = call(WseOpcode::str_get_char, {0, 128, 0}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_get_char ignores an invalid string operand");
    dispatch = call(WseOpcode::str_get_char, {0}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_get_char rejects a missing source operand");

    // str_to_num.
    dispatch = call(WseOpcode::str_to_num, {0, 6}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 123,
           "str_to_num parses an integer");
    dispatch = call(WseOpcode::str_to_num, {0, 11}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 12,
           "str_to_num truncates a fractional value");
    dispatch = call(WseOpcode::str_to_num, {0, 20}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -7,
           "str_to_num truncates toward zero");
    dispatch = call(WseOpcode::str_to_num, {0, 12}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 12,
           "str_to_num parses the numeric prefix");
    dispatch = call(WseOpcode::str_to_num, {0, 13}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 42,
           "str_to_num skips leading whitespace");
    dispatch = call(WseOpcode::str_to_num, {0, 14}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "str_to_num yields zero for a non-numeric string");
    dispatch = call(WseOpcode::str_to_num, {0, 15}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "str_to_num yields zero for the empty string");
    dispatch = call(WseOpcode::str_to_num, {0, 21}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1000,
           "str_to_num parses an exponent");
    dispatch = call(WseOpcode::str_to_num, {0, 11, 1}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1250,
           "str_to_num applies the fixed-point scale");
    dispatch = call(WseOpcode::str_to_num, {0, 19, 1}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 28,
           "str_to_num truncates a scaled fractional value");
    dispatch = call(WseOpcode::str_to_num, {0, 22, 1}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -28,
           "str_to_num truncates a negative scaled value toward zero");
    dispatch = call(WseOpcode::str_to_num, {0, 6, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 123,
           "str_to_num leaves the value unscaled for a zero flag");
    dispatch = call(WseOpcode::str_to_num, {0, 128}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_to_num ignores an invalid string operand");
    dispatch = call(WseOpcode::str_to_num, {0}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_to_num rejects a missing source operand");

    // str_compare.
    dispatch = call(WseOpcode::str_compare, {0, 0, 2}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_compare normalises a smaller left operand to -1");
    dispatch = call(WseOpcode::str_compare, {0, 2, 0}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1,
           "str_compare normalises a larger left operand to 1");
    dispatch = call(WseOpcode::str_compare, {0, 0, 0}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "str_compare reports equal strings as zero");
    dispatch = call(WseOpcode::str_compare, {0, 0, 1}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1,
           "str_compare is case-sensitive by default");
    dispatch = call(WseOpcode::str_compare, {0, 0, 1, 1}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "str_compare folds case when operand 3 is nonzero");
    dispatch = call(WseOpcode::str_compare, {0, 14, 9}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -1,
           "str_compare orders a prefix before the longer string");
    dispatch = call(WseOpcode::str_compare, {0, 9, 14}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1,
           "str_compare orders the longer string after its prefix");
    registers[25] = u"a";
    registers[26] = u"c";
    dispatch = call(WseOpcode::str_compare, {0, 25, 26, 1}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == -2,
           "str_compare keeps the _wcsicmp code-unit difference");
    dispatch = call(WseOpcode::str_compare, {0, 128, 0}, {0, 0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_compare ignores an invalid string operand");
    dispatch = call(WseOpcode::str_compare, {0, 0}, {0, 0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_compare rejects a missing second string");

    // str_split.
    registers[27] = u"a,b,c";
    registers[28] = u",";
    dispatch = call(WseOpcode::str_split, {0, 70, 27, 28}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 3 &&
               registers[70] == u"a" && registers[71] == u"b" &&
               registers[72] == u"c",
           "str_split stores all pieces into consecutive tag-0 registers");

    registers[27] = u",a,,b,";
    dispatch = call(WseOpcode::str_split, {0, 70, 27, 28, 0}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 5 &&
               registers[70].empty() && registers[71] == u"a" &&
               registers[72].empty() && registers[73] == u"b" &&
               registers[74].empty(),
           "str_split preserves empty pieces when skip_empty is zero");
    dispatch = call(WseOpcode::str_split, {0, 70, 27, 28, 1}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 2 &&
               registers[70] == u"a" && registers[71] == u"b",
           "str_split skips empty pieces when operand 4 is nonzero");
    dispatch =
        call(WseOpcode::str_split, {0, 70, 27, 28, 1, 1}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1 &&
               registers[70] == u"a",
           "str_split caps output at max_pieces when 6 operands are present");
    dispatch =
        call(WseOpcode::str_split, {0, 70, 27, 28, 1, 0}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 2 &&
               registers[70] == u"a" && registers[71] == u"b",
           "str_split treats max_pieces == 0 as unlimited");

    registers[27] = u"x,y,z";
    dispatch = call(WseOpcode::str_split, {0, 126, 27, 28}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 2 &&
               registers[126] == u"x" && registers[127] == u"y",
           "str_split clips at tag-0 register 127");

    dispatch = call(WseOpcode::str_split, {0, 80, 1, 23}, {0, 0, 3, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 2 &&
               registers[80] == u"hello" && registers[81] == u"world",
           "str_split reads source from the global string bank");

    registers[27] = u"abc";
    dispatch = call(WseOpcode::str_split, {0, 82, 27, 15, 0}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 1 &&
               registers[82] == u"abc",
           "str_split stores the whole string once on an empty delimiter");
    dispatch = call(WseOpcode::str_split, {0, 83, 15, 15, 1}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled && effect.value == 0,
           "str_split skips an empty string on an empty delimiter with "
           "skip_empty");

    dispatch = call(WseOpcode::str_split, {0, 128, 27, 28}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_split ignores an invalid destination register");
    dispatch = call(WseOpcode::str_split, {0, 70, 128, 28}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_split ignores an invalid source string");
    dispatch = call(WseOpcode::str_split, {0, 70, 27}, {0, 0, 0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_split rejects a missing delimiter operand");

    // str_sort.
    registers[30] = u"delta";
    registers[31] = u"Alpha";
    registers[32] = u"charlie";
    registers[33] = u"bravo";
    registers[34] = u"echo";
    dispatch = call(WseOpcode::str_sort, {30, 5}, {});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[30] == u"Alpha" && registers[31] == u"bravo" &&
               registers[32] == u"charlie" && registers[33] == u"delta" &&
               registers[34] == u"echo",
           "str_sort orders a register range");
    registers[30] = u"delta";
    registers[31] = u"Alpha";
    registers[32] = u"charlie";
    registers[33] = u"bravo";
    registers[34] = u"echo";
    dispatch = call(WseOpcode::str_sort, {30, 5, 0, 1}, {});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[30] == u"echo" && registers[31] == u"delta" &&
               registers[32] == u"charlie" && registers[33] == u"bravo" &&
               registers[34] == u"Alpha",
           "str_sort reverses the range when operand 3 is nonzero");
    registers[30] = u"b";
    registers[31] = u"A";
    registers[32] = u"c";
    dispatch = call(WseOpcode::str_sort, {30, 3, 1}, {});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[30] == u"A" && registers[31] == u"b" &&
               registers[32] == u"c",
           "str_sort folds case when operand 2 is nonzero");
    registers[30] = u"z";
    dispatch = call(WseOpcode::str_sort, {30, 0}, {});
    expect(dispatch == WseDispatch::handled_no_store && registers[30] == u"z",
           "str_sort ignores a non-positive count");
    dispatch = call(WseOpcode::str_sort, {128, 1}, {});
    expect(dispatch == WseDispatch::ignored,
           "str_sort ignores an invalid first register");
    registers[126] = u"b";
    registers[127] = u"a";
    dispatch = call(WseOpcode::str_sort, {126, 5}, {});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[126] == u"a" && registers[127] == u"b",
           "str_sort clips the range to the register table");

    // str_store_lower / str_store_upper.
    dispatch = call(WseOpcode::str_store_lower, {40, 1}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[40] == u"alpha",
           "str_store_lower lower-cases a string register");
    dispatch = call(WseOpcode::str_store_upper, {41, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[41] == u"ALPHA",
           "str_store_upper upper-cases a string register");
    dispatch = call(WseOpcode::str_store_lower, {128, 0}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_lower ignores an invalid destination register");
    dispatch = call(WseOpcode::str_store_lower, {40, 128}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_lower ignores an invalid source register");
    dispatch = call(WseOpcode::str_store_lower, {40}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_store_lower rejects a missing source operand");

    // str_store_trim.
    registers[42] = u"  hi  ";
    dispatch = call(WseOpcode::str_store_trim, {43, 42}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[43] == u"hi",
           "str_store_trim trims both ends by default");
    dispatch = call(WseOpcode::str_store_trim, {43, 42, 1}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[43] == u"hi  ",
           "str_store_trim trims leading whitespace for mode 1");
    dispatch = call(WseOpcode::str_store_trim, {43, 42, 2}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[43] == u"  hi",
           "str_store_trim trims trailing whitespace for mode 2");
    dispatch = call(WseOpcode::str_store_trim, {43, 42, 9}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[43] == u"  hi  ",
           "str_store_trim copies the source for an unknown mode");
    registers[42] = u"\thi\t";
    dispatch = call(WseOpcode::str_store_trim, {43, 42}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[43] == u"hi",
           "str_store_trim treats tabs as whitespace");

    // str_store_replace.
    dispatch = call(WseOpcode::str_store_replace, {44, 9, 10, 14}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[44] == u"aabcaabc",
           "str_store_replace replaces every occurrence");
    dispatch = call(WseOpcode::str_store_replace, {44, 9, 15, 14}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[44] == u"abcabc",
           "str_store_replace leaves the source unchanged for an empty needle");
    dispatch = call(WseOpcode::str_store_replace, {44, 9, 128, 14}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_replace ignores an invalid needle register");
    dispatch = call(WseOpcode::str_store_replace, {44, 9, 10}, {0, 0, 0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_store_replace rejects a missing replacement operand");

    // str_store_substring.
    dispatch = call(WseOpcode::str_store_substring, {45, 9, 2, 3}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[45] == u"cab",
           "str_store_substring extracts a bounded substring");
    dispatch = call(WseOpcode::str_store_substring, {45, 9, 2}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[45] == u"cabc",
           "str_store_substring defaults the length to the end");
    dispatch = call(WseOpcode::str_store_substring, {45, 9}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[45] == u"abcabc",
           "str_store_substring defaults the start to zero");
    registers[45] = u"keep";
    dispatch = call(WseOpcode::str_store_substring, {45, 9, -1, 2}, {0, 0});
    expect(dispatch == WseDispatch::ignored && registers[45] == u"keep",
           "str_store_substring ignores a negative start");
    dispatch = call(WseOpcode::str_store_substring, {45, 9, 2, 99}, {0, 0});
    expect(dispatch == WseDispatch::ignored && registers[45] == u"keep",
           "str_store_substring ignores a range past the end");
    dispatch = call(WseOpcode::str_store_substring, {45, 9, 6, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store && registers[45].empty(),
           "str_store_substring accepts an empty range at the end");

    // str_store_reverse.
    dispatch = call(WseOpcode::str_store_reverse, {46, 9}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[46] == u"cbacba",
           "str_store_reverse reverses the string");
    dispatch = call(WseOpcode::str_store_reverse, {46, 128}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_reverse ignores an invalid source register");

    // str_store_join.
    registers[50] = u"a";
    registers[51] = u"b";
    registers[52] = u"c";
    registers[54] = u"-";
    dispatch = call(WseOpcode::str_store_join, {53, 50, 3, 54}, {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[53] == u"a-b-c",
           "str_store_join inserts the separator between registers");
    dispatch = call(WseOpcode::str_store_join, {53, 50, 3}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[53] == u"abc",
           "str_store_join defaults to an empty separator");
    dispatch = call(WseOpcode::str_store_join, {53, 50, 1}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[53] == u"a",
           "str_store_join joins a single register");
    registers[53] = u"keep";
    dispatch = call(WseOpcode::str_store_join, {53, 50, 0}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[53] == u"keep",
           "str_store_join ignores a non-positive count");
    dispatch = call(WseOpcode::str_store_join, {53, 50, 3, 99}, {0, 0, 0, 3});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[53] == u"abc",
           "str_store_join ignores an invalid separator register");
    dispatch = call(WseOpcode::str_store_join, {53, 128, 1}, {0, 0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_join ignores an invalid start register");
    dispatch = call(WseOpcode::str_store_join, {53, 50}, {0, 0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_store_join rejects a missing count operand");

    // str_store_replace_spaces_with_underscores and the reverse.
    dispatch = call(WseOpcode::str_store_replace_spaces_with_underscores,
                    {60, 5}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[60] == u"a_b",
           "str_store_replace_spaces_with_underscores replaces spaces");
    dispatch = call(WseOpcode::str_store_replace_underscores_with_spaces,
                    {61, 60}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[61] == u"a b",
           "str_store_replace_underscores_with_spaces restores spaces");
    dispatch = call(WseOpcode::str_store_replace_spaces_with_underscores,
                    {128, 5}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_replace_spaces_with_underscores ignores an invalid "
           "destination");
    dispatch = call(WseOpcode::str_store_replace_spaces_with_underscores,
                    {60}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_store_replace_spaces_with_underscores rejects a missing source");
}

void test_string_extended_group() {
    std::u16string registers[128] = {};
    const std::u16string_view global[] = {
        u"message digest",  // 0
        u"xxab12yy",        // 1
        u"[0-9]+",          // 2
        u"42.5",            // 3
    };
    const std::u16string_view local[] = {
        u"ab12",    // 0
        u" -0 ",    // 1
        u"[",       // 2 (invalid regex)
    };
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    fp[0] = 3.25f;
    fp[1] = -1.5f;

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(registers);
    state.strings.global = std::span<const std::u16string_view>(global);
    state.strings.local = std::span<const std::u16string_view>(local);

    WseEffect effect;
    WseDispatch dispatch = WseDispatch::unimplemented;
    const auto call = [&](WseOpcode op, std::initializer_list<std::int32_t> operands,
                          std::initializer_list<std::uint8_t> tags) {
        return run(opcode(op), {}, operands, effect, fp, {}, state, tags);
    };

    // str_store_md5 (4219).
    registers[0] = u"";
    registers[1] = u"abc";
    registers[2] =
        u"1234567890123456789012345678901234567890123456789012345678901234567890"
        u"1234567890";
    registers[3] = u"\u00e9";
    registers[4] = u"\U0001F600";
    dispatch = call(WseOpcode::str_store_md5, {10, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[10] == u"d41d8cd98f00b204e9800998ecf8427e",
           "str_store_md5 computes RFC 1321 empty vector");
    dispatch = call(WseOpcode::str_store_md5, {10, 1}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[10] == u"900150983cd24fb0d6963f7d28e17f72",
           "str_store_md5 computes RFC 1321 abc vector");
    dispatch = call(WseOpcode::str_store_md5, {10, 0}, {0, 3});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[10] == u"f96b697d7cb7938d525a2f31aaf161d0",
           "str_store_md5 reads global bank and computes message digest");
    dispatch = call(WseOpcode::str_store_md5, {10, 2}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[10] == u"57edf4a22be3c955ac49da2e2107b67a",
           "str_store_md5 handles multi-block messages");
    dispatch = call(WseOpcode::str_store_md5, {10, 3}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[10] == u"66ddcd97cfdeabb2f6fb8a999b4bc76f",
           "str_store_md5 encodes BMP code points as UTF-8");
    dispatch = call(WseOpcode::str_store_md5, {10, 4}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[10] == u"2a02eac39d716a70ecf37579185927b6",
           "str_store_md5 encodes UTF-16 surrogate pairs as UTF-8");
    dispatch = call(WseOpcode::str_store_md5, {128, 1}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_md5 ignores an invalid destination");
    dispatch = call(WseOpcode::str_store_md5, {10}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_store_md5 rejects a missing source");

    // str_store_float (4233).
    dispatch = call(WseOpcode::str_store_float, {11, 0}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[11] == u"3.25000",
           "str_store_float defaults precision to 5");
    dispatch = call(WseOpcode::str_store_float, {11, 1, -1}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[11] == u"-1.50000",
           "str_store_float defaults negative precision to 5");
    dispatch = call(WseOpcode::str_store_float, {11, 0, 2}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[11] == u"3.25",
           "str_store_float formats explicit precision");
    dispatch = call(WseOpcode::str_store_float, {11, 0, 0}, {0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[11] == u"3",
           "str_store_float accepts zero precision");
    dispatch = call(WseOpcode::str_store_float, {11, 128}, {0, 0});
    expect(dispatch == WseDispatch::ignored,
           "str_store_float ignores an invalid fp register");
    dispatch = call(WseOpcode::str_store_float, {11}, {0});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_store_float rejects a missing fp register");

    // str_sanitize (4234).
    registers[12] = u"{hello}<world>{a<b>c}";
    dispatch = call(WseOpcode::str_sanitize, {12}, {0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[12] == u"helloworldabc",
           "str_sanitize strips braces and angle brackets");
    dispatch = call(WseOpcode::str_sanitize, {128}, {0});
    expect(dispatch == WseDispatch::ignored,
           "str_sanitize ignores an invalid register");
    dispatch = call(WseOpcode::str_sanitize, {}, {});
    expect(dispatch == WseDispatch::operand_count_out_of_range,
           "str_sanitize rejects a missing operand");

    // str_is_integer (4236).
    registers[13] = u" 123\t";
    registers[14] = u"-42";
    registers[15] = u"-";
    registers[16] = u"+12";
    registers[17] = u"12a";
    registers[18] = u"   ";
    expect(call(WseOpcode::str_is_integer, {13}, {0}) ==
               WseDispatch::handled_no_store,
           "str_is_integer trims whitespace");
    expect(call(WseOpcode::str_is_integer, {14}, {0}) ==
               WseDispatch::handled_no_store,
           "str_is_integer accepts negative integers");
    expect(call(WseOpcode::str_is_integer, {1}, {0x16}) ==
               WseDispatch::handled_no_store,
           "str_is_integer reads local bank strings");
    expect(call(WseOpcode::str_is_integer, {15}, {0}) ==
               WseDispatch::condition_false,
           "str_is_integer rejects bare minus");
    expect(call(WseOpcode::str_is_integer, {16}, {0}) ==
               WseDispatch::condition_false,
           "str_is_integer rejects leading plus");
    expect(call(WseOpcode::str_is_integer, {17}, {0}) ==
               WseDispatch::condition_false,
           "str_is_integer rejects trailing letters");
    expect(call(WseOpcode::str_is_integer, {18}, {0}) ==
               WseDispatch::condition_false,
           "str_is_integer rejects whitespace-only string");
    expect(call(WseOpcode::str_is_integer, {99}, {0x16}) ==
               WseDispatch::ignored,
           "str_is_integer ignores an invalid local string");
    expect(call(WseOpcode::str_is_integer, {}, {}) ==
               WseDispatch::operand_count_out_of_range,
           "str_is_integer rejects a missing operand");

    // str_store_server_password_rcon (4238).
    registers[19] = u"keep";
    dispatch = call(WseOpcode::str_store_server_password_rcon, {19}, {0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[19] == u"keep",
           "str_store_server_password_rcon is a v1.1.5.5 no-op");

    // str_regex_match (4240) and str_regex_search (4241).
    registers[20] = u"ab[0-9]+";
    expect(call(WseOpcode::str_regex_match, {1, 20}, {3, 0}) ==
               WseDispatch::condition_false,
           "str_regex_match requires a full-string match");
    expect(call(WseOpcode::str_regex_search, {1, 20}, {3, 0}) ==
               WseDispatch::handled_no_store,
           "str_regex_search finds a substring match");
    expect(call(WseOpcode::str_regex_match, {0, 20}, {0x16, 0}) ==
               WseDispatch::handled_no_store,
           "str_regex_match matches the full string");
    expect(call(WseOpcode::str_regex_match, {1, 2}, {3, 0x16}) ==
               WseDispatch::condition_false,
           "str_regex_match catches invalid regex syntax");
    expect(call(WseOpcode::str_regex_search, {1, 2}, {3, 0x16}) ==
               WseDispatch::condition_false,
           "str_regex_search catches invalid regex syntax");
    expect(call(WseOpcode::str_regex_match, {99, 20}, {3, 0}) ==
               WseDispatch::ignored,
           "str_regex_match ignores invalid source register");
    expect(call(WseOpcode::str_regex_match, {0}, {0}) ==
               WseDispatch::operand_count_out_of_range,
           "str_regex_match rejects missing pattern");

    // str_regex_get_matches (4242).
    registers[21] = u"a10 b20 c30";
    registers[33] = u"sentinel";
    dispatch = call(WseOpcode::str_regex_get_matches, {30, 21, 2}, {0, 0, 3});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[30] == u"10" && registers[31] == u"20" &&
               registers[32] == u"30" && registers[33] == u"sentinel",
           "str_regex_get_matches stores consecutive matches");
    registers[36] = u"sentinel";
    dispatch = call(WseOpcode::str_regex_get_matches, {34, 21, 2, 2},
                    {0, 0, 3, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[34] == u"10" && registers[35] == u"20" &&
               registers[36] == u"sentinel",
           "str_regex_get_matches respects optional max_matches");
    dispatch = call(WseOpcode::str_regex_get_matches, {127, 21, 2}, {0, 0, 3});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[127] == u"10",
           "str_regex_get_matches stops at register 127");
    registers[37] = u"unchanged";
    dispatch =
        call(WseOpcode::str_regex_get_matches, {37, 21, 2}, {0, 0, 0x16});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[37] == u"unchanged",
           "str_regex_get_matches catches invalid regex syntax");
    expect(call(WseOpcode::str_regex_get_matches, {128, 21, 2}, {0, 0, 3}) ==
               WseDispatch::ignored,
           "str_regex_get_matches ignores invalid destination register");
    expect(call(WseOpcode::str_regex_get_matches, {30, 21}, {0, 0}) ==
               WseDispatch::operand_count_out_of_range,
           "str_regex_get_matches rejects missing pattern");

    // str_store_regex_replace (4243).
    registers[22] = u"id=10, n=20";
    registers[23] = u"#";
    registers[24] = u"(\\w+)=(\\d+)";
    registers[25] = u"$2:$1";
    dispatch = call(WseOpcode::str_store_regex_replace, {40, 22, 2, 23},
                    {0, 0, 3, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[40] == u"id=#, n=#",
           "str_store_regex_replace replaces all matches");
    dispatch = call(WseOpcode::str_store_regex_replace, {41, 22, 24, 25},
                    {0, 0, 0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[41] == u"10:id, 20:n",
           "str_store_regex_replace supports backreferences");
    registers[42] = u"untouched";
    dispatch = call(WseOpcode::str_store_regex_replace, {42, 22, 2, 23},
                    {0, 0, 0x16, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[42] == u"untouched",
           "str_store_regex_replace catches invalid regex syntax");
    expect(call(WseOpcode::str_store_regex_replace, {128, 22, 2, 23},
                {0, 0, 3, 0}) == WseDispatch::ignored,
           "str_store_regex_replace ignores invalid destination");
    expect(call(WseOpcode::str_store_regex_replace, {40, 22, 2}, {0, 0, 3}) ==
               WseDispatch::operand_count_out_of_range,
           "str_store_regex_replace rejects missing replacement");

    // str_decode_url (4244).
    registers[26] = u"hello%20world+a%2Fb%3F%c3%a9%GG%2";
    registers[27] = u"ab%00cd";
    dispatch = call(WseOpcode::str_decode_url, {50, 26}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store &&
               registers[50] == u"hello world+a/b?\u00e9%GG%2",
           "str_decode_url decodes percent bytes and UTF-8 while keeping +");
    dispatch = call(WseOpcode::str_decode_url, {51, 27}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store && registers[51] == u"ab",
           "str_decode_url terminates at percent-encoded NUL");
    expect(call(WseOpcode::str_decode_url, {128, 26}, {0, 0}) ==
               WseDispatch::ignored,
           "str_decode_url ignores invalid destination");
    expect(call(WseOpcode::str_decode_url, {50}, {0}) ==
               WseDispatch::operand_count_out_of_range,
           "str_decode_url rejects missing source");

    // fld_str (4401) using state.strings.
    dispatch = call(WseOpcode::fld_str, {2, 3}, {0, 3});
    expect(dispatch == WseDispatch::handled_no_store && close(fp[2], 42.5f),
           "fld_str parses a global string into an fp register");
    registers[28] = u"-12.25";
    dispatch = call(WseOpcode::fld_str, {3, 28}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store && close(fp[3], -12.25f),
           "fld_str parses signed floats from tag-0 registers");
    registers[29] = u"not_a_number";
    fp[4] = 99.0f;
    dispatch = call(WseOpcode::fld_str, {4, 29}, {0, 0});
    expect(dispatch == WseDispatch::handled_no_store && fp[4] == 0.0f,
           "fld_str matches atof by storing 0 on non-numeric input");
    expect(call(WseOpcode::fld_str, {128, 28}, {0, 0}) == WseDispatch::ignored,
           "fld_str ignores invalid fp register");
    expect(call(WseOpcode::fld_str, {2}, {0}) ==
               WseDispatch::operand_count_out_of_range,
           "fld_str rejects missing source string");
}

void test_options_group() {
    WseEffect effect;
    wse::vm::WseOptionsState options;
    WseVmState state;
    state.options = &options;

    expect(run(opcode(WseOpcode::options_get_verbose_casualties), {}, {},
               effect) == WseDispatch::handled &&
               effect.value == 0,
           "options_get_verbose_casualties defaults false with null options");
    expect(run(opcode(WseOpcode::options_set_verbose_casualties), {}, {1},
               effect) == WseDispatch::ignored,
           "options_set_verbose_casualties ignores null options state");

    const struct {
        WseOpcode get;
        WseOpcode set;
        const char* label;
    } kCases[] = {
        {WseOpcode::options_get_verbose_casualties,
         WseOpcode::options_set_verbose_casualties, "verbose_casualties"},
        {WseOpcode::options_get_cheat_mode, WseOpcode::options_set_cheat_mode,
         "cheat_mode"},
        {WseOpcode::options_get_realistic_headshots,
         WseOpcode::options_set_realistic_headshots, "realistic_headshots"},
    };
    for (const auto& entry : kCases) {
        expect(run(opcode(entry.get), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
                   effect.value == 0,
               entry.label);
        expect(run(opcode(entry.set), {}, {7}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
                   !effect.store_value,
               entry.label);
        expect(run(opcode(entry.get), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
                   effect.value == 1,
               entry.label);
        expect(run(opcode(entry.set), {}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store,
               entry.label);
        expect(run(opcode(entry.get), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
                   effect.value == 0,
               entry.label);
        expect(run(opcode(entry.set), {}, {}, effect, {}, {}, state) ==
                   WseDispatch::operand_count_out_of_range,
               entry.label);
    }
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
    test_register_group();
    test_script_context_group();
    test_string_group();
    test_string_extended_group();
    test_options_group();
    test_operand_counts();
    test_unimplemented_and_mask();
    test_fp_load_store();
    test_fp_compare();
    test_fp_arithmetic();
    test_fp_math();
    test_fp_errors();
    test_descriptions();

    if (failures != 0) {
        std::printf("wse tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("wse tests: PASS\n");
    return 0;
}
