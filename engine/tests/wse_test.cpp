// Host tests for the portable executeWSE handlers.
//
// Build/run: powershell -File engine/build_host.ps1

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    expect(opcode(WseOpcode::feval) == 4437, "feval opcode value");
    expect(opcode(WseOpcode::fadd) == 4438, "fadd opcode value");
    expect(opcode(WseOpcode::array_create) == 5000, "array_create opcode value");
    expect(opcode(WseOpcode::array_le) == 5022, "array_le opcode value");
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

    dispatch = run(opcode(WseOpcode::lua_get_top), {1}, {}, effect);
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

void test_script_state_and_position_group() {
    std::u16string str_regs[128] = {};
    const std::u16string_view global_strs[] = {
        u"marker_global",  // 0
        u"bad_pos_val",    // 1
    };
    const std::u16string_view local_strs[] = {
        u"marker_local",   // 0
    };
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<std::int64_t, 16> return_vals{};
    std::int32_t num_returns = 0;
    std::array<wse::vm::WseTransform, 128> positions{};
    std::array<wse::vm::WseAppTimer, 128> timers{};
    std::array<std::int64_t, 3> timers_total{1000000LL, 2000000LL, 5000000LL};
    wse::vm::WseMersenneTwister rng{};
    std::int32_t rand_seed = 0;
    bool profiler_recording = false;
    std::vector<std::string> profiler_markers{};

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(str_regs);
    state.strings.global = std::span<const std::u16string_view>(global_strs);
    state.strings.local = std::span<const std::u16string_view>(local_strs);
    state.return_values = return_vals;
    state.return_values_out = return_vals;
    state.num_return_values = 0;
    state.num_return_values_out = &num_returns;
    state.positions = positions;
    state.timers = timers;
    state.timers_total = timers_total;
    state.rng = &rng;
    state.rand_seed = &rand_seed;
    state.profiler_recording = &profiler_recording;
    state.profiler_markers = &profiler_markers;

    WseEffect effect{};
    const auto call = [&](WseOpcode op, std::initializer_list<std::int64_t> wide_args,
                          std::initializer_list<std::uint8_t> tag_args = {}) {
        std::vector<std::int64_t> wide(wide_args);
        std::vector<std::int32_t> narrow(wide.size());
        for (std::size_t i = 0; i < wide.size(); ++i) {
            narrow[i] = static_cast<std::int32_t>(wide[i]);
        }
        std::vector<std::uint8_t> tags(tag_args);
        WseOperands operands;
        operands.wide = wide;
        operands.narrow = narrow;
        operands.tags = tags;
        operands.fp = fp;
        operands.registers = int_regs;
        operands.state = state;
        operands.state.return_values = return_vals;
        operands.state.num_return_values = num_returns;
        effect = WseEffect{0x1234, true};
        return execute_wse(opcode(op), operands, effect);
    };

    // 3015 return_values -> 3016 store_num_return_values / 3017 store_return_value.
    expect(call(WseOpcode::return_values, {11, 22, 33}) ==
                   WseDispatch::handled_no_store &&
               num_returns == 3 && return_vals[0] == 11 &&
               return_vals[2] == 33,
           "return_values writes up to 16 int64 slots");
    expect(call(WseOpcode::store_num_return_values, {0}) ==
                   WseDispatch::handled &&
               effect.value == 3,
           "store_num_return_values reads updated count");
    expect(call(WseOpcode::store_return_value, {0, 2}) ==
                   WseDispatch::handled &&
               effect.value == 22,
           "store_return_value reads 1-based slot after return_values");
    expect(call(WseOpcode::return_values,
                {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18}) ==
                   WseDispatch::handled_no_store &&
               num_returns == 16 && return_vals[15] == 16,
           "return_values clamps operand list to 16 slots");
    expect(call(WseOpcode::store_return_value, {0, 17}) ==
                   WseDispatch::ignored &&
               effect.value == 0x1234,
           "store_return_value rejects slot 17 beyond 16");
    expect(call(WseOpcode::return_values, {}) ==
                   WseDispatch::handled_no_store &&
               num_returns == 0,
           "return_values accepts zero operands and clears count");

    // 3020 mtsrand & 3021 mtrand.
    expect(call(WseOpcode::mtsrand, {}) == WseDispatch::operand_count_out_of_range,
           "mtsrand requires 1 operand");
    expect(call(WseOpcode::mtrand, {0, 10}) ==
               WseDispatch::operand_count_out_of_range,
           "mtrand requires 3 operands");
    expect(call(WseOpcode::mtsrand, {1337}) == WseDispatch::handled_no_store,
           "mtsrand seeds MT19937");
    std::vector<std::int64_t> seq1;
    for (int i = 0; i < 6; ++i) {
        expect(call(WseOpcode::mtrand, {0, -5, 15}) == WseDispatch::handled &&
                   effect.value >= -5 && effect.value < 15,
               "mtrand produces values in [min, max)");
        seq1.push_back(effect.value);
    }
    expect(call(WseOpcode::mtsrand, {1337}) == WseDispatch::handled_no_store,
           "mtsrand re-seeds reproducibly");
    // Edge case: min == max - 1 (span == 0) returns min without advancing RNG.
    expect(call(WseOpcode::mtrand, {0, 42, 43}) == WseDispatch::handled &&
               effect.value == 42,
           "mtrand handles min == max - 1 without drawing");
    for (int i = 0; i < 6; ++i) {
        expect(call(WseOpcode::mtrand, {0, -5, 15}) == WseDispatch::handled &&
                   effect.value == seq1[static_cast<std::size_t>(i)],
               "mtrand min == max - 1 preserves exact PRNG stream");
    }
    // Edge case: min == max wraps unsigned span to 0xffffffffU and draws once cleanly.
    expect(call(WseOpcode::mtrand, {0, 5, 5}) == WseDispatch::handled,
           "mtrand min == max completes without hanging");
    {
        WseVmState no_rng = state;
        no_rng.rng = nullptr;
        std::array<std::int32_t, 3> args{0, 1, 10};
        WseOperands ops;
        ops.narrow = args;
        ops.state = no_rng;
        expect(execute_wse(opcode(WseOpcode::mtrand), ops, effect) ==
                   WseDispatch::ignored,
               "mtrand ignores missing RNG pointer");
    }

    // 3022 get_time (UTC & local).
    expect(call(WseOpcode::get_time, {}) ==
               WseDispatch::operand_count_out_of_range,
           "get_time requires 1 operand");
    const std::time_t sys_before = std::time(nullptr);
    expect(call(WseOpcode::get_time, {0}) == WseDispatch::handled,
           "get_time UTC succeeds");
    const std::int64_t utc_val = effect.value;
    const std::time_t sys_after = std::time(nullptr);
    expect(utc_val >= static_cast<std::int32_t>(sys_before) - 2 &&
               utc_val <= static_cast<std::int32_t>(sys_after) + 2,
           "get_time returns current UTC unix epoch seconds");
    expect(call(WseOpcode::get_time, {0, 1}) == WseDispatch::handled,
           "get_time local succeeds");
    const std::int64_t local_val = effect.value;
    const std::time_t mid_t = static_cast<std::time_t>(utc_val);
    std::tm lt{};
    bool has_lt = false;
#ifdef _WIN32
    has_lt = localtime_s(&lt, &mid_t) == 0;
#else
    has_lt = localtime_r(&mid_t, &lt) != nullptr;
#endif
    if (has_lt) {
        const std::int64_t expected_local =
            wse::vm::wse_days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1,
                                         lt.tm_mday) *
                86400LL +
            static_cast<std::int64_t>(lt.tm_hour) * 3600LL +
            static_cast<std::int64_t>(lt.tm_min) * 60LL +
            static_cast<std::int64_t>(lt.tm_sec);
        expect(local_val >= expected_local - 2 &&
                   local_val <= expected_local + 2,
               "get_time(local=1) applies local timezone offset");
    }

    // 3027 timer_reset & 3028 timer_get_elapsed_time (100kHz ticks).
    expect(call(WseOpcode::timer_reset, {}) ==
               WseDispatch::operand_count_out_of_range,
           "timer_reset requires 1 operand");
    expect(call(WseOpcode::timer_get_elapsed_time, {0}) ==
               WseDispatch::operand_count_out_of_range,
           "timer_get_elapsed_time requires 2 operands");
    expect(call(WseOpcode::timer_reset, {0, 1}) ==
                   WseDispatch::handled_no_store &&
               timers[0].kind == 0 && timers[0].start_ticks == 1000000LL,
           "timer_reset nonzero op1 selects app clock (kind 0)");
    expect(call(WseOpcode::timer_reset, {1}) ==
                   WseDispatch::handled_no_store &&
               timers[1].kind == 2 && timers[1].start_ticks == 5000000LL,
           "timer_reset default/0 op1 selects game clock (kind 2)");
    timers_total[0] += 150000LL;  // 1.500s -> rglRound64(1.5f) == 1
    expect(call(WseOpcode::timer_get_elapsed_time, {0, 0}) ==
                   WseDispatch::handled &&
               effect.value == 1,
           "timer_get_elapsed_time rounds exact 1.5s down matching rglRound64");
    timers_total[0] += 1000LL;    // 1.510s -> 2
    expect(call(WseOpcode::timer_get_elapsed_time, {0, 0}) ==
                   WseDispatch::handled &&
               effect.value == 2,
           "timer_get_elapsed_time uses 100kHz scale");
    timers_total[2] += 325000LL;  // 3.25s -> 3
    expect(call(WseOpcode::timer_get_elapsed_time, {0, 1}) ==
                   WseDispatch::handled &&
               effect.value == 3,
           "timer_get_elapsed_time tracks kind 2 clock independently");
    expect(call(WseOpcode::timer_reset, {128, 1}) == WseDispatch::ignored &&
               call(WseOpcode::timer_get_elapsed_time, {0, -1}) ==
                   WseDispatch::ignored,
           "timer ops ignore out-of-range timer slot");

    // 3034 set_random_seed.
    expect(call(WseOpcode::set_random_seed, {}) ==
               WseDispatch::operand_count_out_of_range,
           "set_random_seed requires 1 operand");
    expect(call(WseOpcode::set_random_seed, {98765}) ==
                   WseDispatch::handled_no_store &&
               rand_seed == 98765,
           "set_random_seed updates state.rand_seed");

    // 3040..3043 profiler_start/stop/is_recording/mark.
    expect(call(WseOpcode::profiler_is_recording, {}) ==
               WseDispatch::condition_false,
           "profiler_is_recording initially false");
    str_regs[0] = u"ignored_marker";
    expect(call(WseOpcode::profiler_mark, {0}) ==
                   WseDispatch::handled_no_store &&
               profiler_markers.empty(),
           "profiler_mark ignores markers when not recording");
    expect(call(WseOpcode::profiler_start, {}) ==
                   WseDispatch::handled_no_store &&
               profiler_recording,
           "profiler_start enables recording");
    expect(call(WseOpcode::profiler_is_recording, {}) ==
               WseDispatch::handled_no_store,
           "profiler_is_recording returns true while active");
    str_regs[0] = u"frame_0";
    expect(call(WseOpcode::profiler_mark, {0}, {0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::profiler_mark, {0}, {3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::profiler_mark, {0}, {0x16}) ==
                   WseDispatch::handled_no_store,
           "profiler_mark records across tag banks");
    expect(profiler_markers.size() == 3U && profiler_markers[0] == "frame_0" &&
               profiler_markers[1] == "marker_global" &&
               profiler_markers[2] == "marker_local",
           "profiler_mark appends UTF-8 markers");
    expect(call(WseOpcode::profiler_stop, {}) ==
                   WseDispatch::handled_no_store &&
               !profiler_recording &&
               call(WseOpcode::profiler_is_recording, {}) ==
                   WseDispatch::condition_false,
           "profiler_stop disables recording");
    expect(call(WseOpcode::profiler_mark, {0}) ==
                   WseDispatch::handled_no_store &&
               profiler_markers.size() == 3U,
           "profiler_mark stops recording markers after profiler_stop");
    expect(call(WseOpcode::profiler_mark, {}) ==
               WseDispatch::operand_count_out_of_range,
           "profiler_mark requires 1 operand");

    // 4100 position_get_vector_to_position, 4102..4104, 4402..4404 fld_pos_*.
    positions[1].o = {1.0f, 2.0f, 3.0f, 1.0f};
    positions[2].o = {1.0f, 6.0f, 6.0f, 1.0f};
    expect(call(WseOpcode::position_get_vector_to_position, {0, 0, 1, 2}) ==
                   WseDispatch::handled &&
               effect.value == 500,
           "position_get_vector_to_position returns scaled distance");
    expect(close(positions[0].o.x, 1.0f) && close(positions[0].o.y, 2.0f) &&
               close(positions[0].o.z, 3.0f) &&
               close(positions[0].f.x, 0.0f) &&
               close(positions[0].f.y, 0.8f) &&
               close(positions[0].f.z, 0.6f),
           "position_get_vector_to_position sets origin and aimed forward basis");
    expect(close(positions[0].s.length(), 1.0f) &&
               close(positions[0].u.length(), 1.0f) &&
               close(positions[0].s.dot(positions[0].f), 0.0f) &&
               close(positions[0].s.dot(positions[0].u), 0.0f) &&
               close(positions[0].f.dot(positions[0].u), 0.0f),
           "position_get_vector_to_position yields orthonormal basis");
    // Aliasing test: dst == pos1.
    positions[3].o = {3.0f, 4.0f, 12.0f, 1.0f};
    positions[4].o = {0.0f, 0.0f, 0.0f, 1.0f};
    expect(call(WseOpcode::position_get_vector_to_position, {0, 3, 3, 4}) ==
                   WseDispatch::handled &&
               effect.value == 1300 && close(positions[3].o.x, 3.0f) &&
               close(positions[3].f.x, -3.0f / 13.0f) &&
               close(positions[3].f.y, -4.0f / 13.0f) &&
               close(positions[3].f.z, -12.0f / 13.0f),
           "position_get_vector_to_position handles dst == pos1 aliasing");
    // 4102 position_get_length.
    positions[5].o = {3.0f, 4.0f, 12.0f, 1.0f};
    expect(call(WseOpcode::position_get_length, {0, 5}) ==
                   WseDispatch::handled &&
               effect.value == 1300,
           "position_get_length returns fixed-point vector length");
    // 4103 get_dot_product_of_positions.
    positions[6].o = {1.0f, 2.0f, 3.0f, 1.0f};
    positions[7].o = {4.0f, -5.0f, 6.0f, 1.0f};
    expect(call(WseOpcode::get_dot_product_of_positions, {0, 6, 7}) ==
                   WseDispatch::handled &&
               effect.value == 1200,
           "get_dot_product_of_positions computes fixed-point dot product");
    // 4104 get_cross_product_of_positions (including aliasing dst == pos1).
    positions[8].o = {1.0f, 0.0f, 0.0f, 1.0f};
    positions[9].o = {0.0f, 2.0f, 0.0f, 1.0f};
    expect(call(WseOpcode::get_cross_product_of_positions, {8, 8, 9}) ==
                   WseDispatch::handled_no_store &&
               close(positions[8].o.x, 0.0f) &&
               close(positions[8].o.y, 0.0f) &&
               close(positions[8].o.z, 2.0f),
           "get_cross_product_of_positions handles cross product with aliasing");
    // 4402..4404 fld_pos_{x,y,z}.
    positions[10].o = {-1.25f, 3.5f, 9.75f, 1.0f};
    expect(call(WseOpcode::fld_pos_x, {4, 10}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::fld_pos_y, {5, 10}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::fld_pos_z, {6, 10}) ==
                   WseDispatch::handled_no_store &&
               close(fp[4], -1.25f) && close(fp[5], 3.5f) &&
               close(fp[6], 9.75f),
           "fld_pos_x/y/z copy origin coordinates to fp registers");
    expect(call(WseOpcode::fld_pos_x, {128, 10}) == WseDispatch::ignored &&
               call(WseOpcode::fld_pos_y, {4, 128}) == WseDispatch::ignored &&
               call(WseOpcode::position_get_length, {0, -1}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::get_dot_product_of_positions, {0, 1, 128}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::get_cross_product_of_positions, {128, 1, 2}) ==
                   WseDispatch::ignored,
           "position/fld_pos ops ignore invalid registers");
    expect(call(WseOpcode::fld_pos_z, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::position_get_vector_to_position, {0, 1, 2}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::position_get_length, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::get_dot_product_of_positions, {0, 1}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::get_cross_product_of_positions, {0, 1}) ==
                   WseDispatch::operand_count_out_of_range,
           "position/fld_pos ops validate operand counts");
}

void test_dictionary_group() {
    namespace fs = std::filesystem;
    const fs::path temp_dir =
        fs::temp_directory_path() / "wse2_dict_unit_test_tmp";
    const fs::path outside_escape =
        fs::temp_directory_path() / "wse2_dict_escape_forbidden.wsedict";
    struct TempGuard {
        fs::path dir;
        fs::path escape;
        ~TempGuard() {
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::remove(escape, ec);
        }
    } guard{temp_dir, outside_escape};
    {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::remove(outside_escape, ec);
        fs::create_directories(temp_dir, ec);
        expect(!ec, "created temporary directory for dictionary file tests");
    }

    std::u16string str_regs[128] = {};
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};
    wse::vm::WseDictionaryStore store;
    store.root_dir = temp_dir;

    WseVmState state;
    state.strings.registers = std::span<std::u16string>(str_regs);
    state.positions = positions;
    state.dictionaries = &store;

    WseEffect effect{};
    const auto call = [&](WseOpcode op, std::initializer_list<std::int64_t> wide_args,
                          std::initializer_list<std::uint8_t> tag_args = {}) {
        std::vector<std::int64_t> wide(wide_args);
        std::vector<std::int32_t> narrow(wide.size());
        for (std::size_t i = 0; i < wide.size(); ++i) {
            narrow[i] = static_cast<std::int32_t>(wide[i]);
        }
        std::vector<std::uint8_t> tags(tag_args);
        WseOperands operands;
        operands.wide = wide;
        operands.narrow = narrow;
        operands.tags = tags;
        operands.fp = fp;
        operands.registers = int_regs;
        operands.state = state;
        effect = WseEffect{0x1234, true};
        return execute_wse(opcode(op), operands, effect);
    };

    // 3200 dict_create & 3201 dict_free (LIFO free-ID reuse).
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 0,
           "dict_create assigns id 0");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 1,
           "dict_create assigns id 1");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 2,
           "dict_create assigns id 2");
    expect(call(WseOpcode::dict_free, {1}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {-1}) == WseDispatch::handled_no_store,
           "dict_free ignores invalid or already-freed IDs");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 0,
           "dict_create reuses most recently freed id (LIFO)");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 1,
           "dict_create reuses earlier freed id next");
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 3,
           "dict_create allocates fresh slot once free list is empty");

    // 3206/3207/3208/3210..3214/3221/3205 basic CRUD & iteration.
    expect(call(WseOpcode::dict_is_empty, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_is_empty, {99}) ==
                   WseDispatch::handled_no_store,
           "dict_is_empty returns true for empty and invalid IDs");
    str_regs[0] = u"beta";
    str_regs[1] = u"hello";
    str_regs[2] = u"alpha";
    str_regs[3] = u"-42";
    str_regs[4] = u"gamma";
    str_regs[5] = u"world\u00e9";
    str_regs[6] = u"missing";
    str_regs[7] = u"fallback_str";
    expect(call(WseOpcode::dict_set_str, {0, 0, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_int, {0, 2, -42}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {0, 4, 5}) ==
                   WseDispatch::handled_no_store,
           "dict_set_str and dict_set_int store entries");
    expect(call(WseOpcode::dict_is_empty, {0}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_get_size, {0, 0}) == WseDispatch::handled &&
               effect.value == 3,
           "dict_is_empty and dict_get_size reflect populated entries");
    expect(call(WseOpcode::dict_has_key, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_has_key, {0, 6}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_has_key, {99, 0}) ==
                   WseDispatch::condition_false,
           "dict_has_key distinguishes present, missing, and invalid dicts");
    expect(call(WseOpcode::dict_get_str, {10, 0, 4}) ==
                   WseDispatch::handled_no_store &&
               str_regs[10] == u"world\u00e9" &&
               call(WseOpcode::dict_get_str, {11, 0, 6, 7}) ==
                   WseDispatch::handled_no_store &&
               str_regs[11] == u"fallback_str" &&
               call(WseOpcode::dict_get_str, {12, 0, 6}) ==
                   WseDispatch::handled_no_store &&
               str_regs[12].empty(),
           "dict_get_str reads UTF-8 values and applies default/empty fallback");
    expect(call(WseOpcode::dict_get_int, {0, 0, 2}) == WseDispatch::handled &&
               effect.value == -42 &&
               call(WseOpcode::dict_get_int, {0, 0, 6, 77}) ==
                   WseDispatch::handled &&
               effect.value == 77 &&
               call(WseOpcode::dict_get_int, {0, 0, 0, 77}) ==
                   WseDispatch::handled &&
               effect.value == 0,
           "dict_get_int parses ints, uses default on missing, 0 on non-int");
    expect(call(WseOpcode::dict_get_key_by_iterator, {20, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               str_regs[20] == u"alpha" &&
               call(WseOpcode::dict_get_key_by_iterator, {21, 0, 1}) ==
                   WseDispatch::handled_no_store &&
               str_regs[21] == u"beta" &&
               call(WseOpcode::dict_get_key_by_iterator, {22, 0, 2}) ==
                   WseDispatch::handled_no_store &&
               str_regs[22] == u"gamma" &&
               call(WseOpcode::dict_get_key_by_iterator, {23, 0, 3}) ==
                   WseDispatch::handled_no_store &&
               str_regs[23].empty(),
           "dict_get_key_by_iterator visits keys in sorted order");
    expect(call(WseOpcode::dict_erase, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_has_key, {0, 0}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_get_size, {0, 0}) == WseDispatch::handled &&
               effect.value == 2,
           "dict_erase removes target key only");

    // 3215 dict_get_pos & 3216 dict_set_pos (9-float fallback & orthonormalization).
    positions[0] = wse::vm::WseTransform::identity();
    positions[0].o = {10.5f, -20.25f, 30.75f, 1.0f};
    positions[0].rotate_z(0.6f);
    positions[0].rotate_x(0.3f);
    str_regs[30] = u"pose";
    expect(call(WseOpcode::dict_set_pos, {0, 30, 0}) ==
               WseDispatch::handled_no_store,
           "dict_set_pos serializes 9 floats");
    expect(call(WseOpcode::dict_get_pos, {1, 0, 30}) ==
                   WseDispatch::handled_no_store &&
               close(positions[1].o.x, 10.5f) &&
               close(positions[1].o.y, -20.25f) &&
               close(positions[1].o.z, 30.75f) &&
               close(positions[1].s.x, positions[0].s.x) &&
               close(positions[1].f.y, positions[0].f.y) &&
               close(positions[1].u.z, positions[0].u.z),
           "dict_get_pos round-trips position transform");
    // Non-orthonormal input gets orthonormalized:
    str_regs[31] = u"skewed";
    str_regs[32] = u"1,2,3,0,2,0,0,1,3";
    expect(call(WseOpcode::dict_set_str, {0, 31, 32}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_pos, {2, 0, 31}) ==
                   WseDispatch::handled_no_store &&
               close(positions[2].o.x, 1.0f) &&
               close(positions[2].o.y, 2.0f) &&
               close(positions[2].o.z, 3.0f) &&
               close(positions[2].s.length(), 1.0f) &&
               close(positions[2].f.length(), 1.0f) &&
               close(positions[2].u.length(), 1.0f) &&
               close(positions[2].s.dot(positions[2].f), 0.0f) &&
               close(positions[2].f.dot(positions[2].u), 0.0f),
           "dict_get_pos orthonormalizes parsed f/u vectors");
    // Fallback when token count != 9 or key missing:
    positions[3].o = {7.0f, 8.0f, 9.0f, 1.0f};
    str_regs[33] = u"short_pos";
    str_regs[34] = u"1,2,3";
    expect(call(WseOpcode::dict_set_str, {0, 33, 34}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_pos, {4, 0, 33, 3}) ==
                   WseDispatch::handled_no_store &&
               close(positions[4].o.x, 7.0f) &&
               close(positions[4].o.y, 8.0f) &&
               close(positions[4].o.z, 9.0f) &&
               call(WseOpcode::dict_get_pos, {4, 0, 33}) ==
                   WseDispatch::handled_no_store &&
               close(positions[4].o.x, 0.0f) && close(positions[4].s.x, 1.0f),
           "dict_get_pos uses fallback/identity when tokens != 9");

    // 3203 dict_load_dict mode semantics (0 is no-op, 1 overwrites, 2 inserts missing).
    str_regs[40] = u"k_keep";
    str_regs[41] = u"v_keep";
    str_regs[42] = u"k_shared";
    str_regs[43] = u"v_old";
    str_regs[44] = u"v_new";
    str_regs[45] = u"k_added";
    str_regs[46] = u"v_added";
    expect(call(WseOpcode::dict_set_str, {1, 40, 41}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {1, 42, 43}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {2, 42, 44}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {2, 45, 46}) ==
                   WseDispatch::handled_no_store,
           "populated source and dest dicts for dict_load_dict");
    expect(call(WseOpcode::dict_load_dict, {1, 2, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_has_key, {1, 45}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::dict_get_str, {50, 1, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_old",
           "dict_load_dict mode 0 is a no-op");
    expect(call(WseOpcode::dict_load_dict, {1, 2, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {50, 1, 45}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_added" &&
               call(WseOpcode::dict_get_str, {50, 1, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_old",
           "dict_load_dict mode 2 inserts only missing keys");
    expect(call(WseOpcode::dict_load_dict, {1, 2, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {50, 1, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[50] == u"v_new" &&
               call(WseOpcode::dict_has_key, {1, 40}) ==
                   WseDispatch::handled_no_store,
           "dict_load_dict mode 1 overwrites collisions while preserving existing keys");

    // 3204 dict_save, 3202 dict_load_file, 3209 dict_delete_file (.wsedict and .ini).
    str_regs[60] = u"bin_dict";
    str_regs[61] = u"ini_dict";
    expect(call(WseOpcode::dict_save, {1, 60}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "bin_dict.wsedict"),
           "dict_save writes binary .wsedict by default");
    expect(call(WseOpcode::dict_save, {1, 61, 1}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "ini_dict.ini"),
           "dict_save(format=1) writes .ini file");
    expect(call(WseOpcode::dict_clear, {3}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_set_str, {3, 40, 46}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_load_file, {3, 60, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {51, 3, 40}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_keep" &&
               call(WseOpcode::dict_get_str, {51, 3, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_new",
           "dict_load_file(mode=0) clears and loads binary .wsedict");
    // Verify mode 2 vs mode 1 on INI and binary:
    expect(call(WseOpcode::dict_set_str, {3, 42, 43}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_erase, {3, 45}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_load_file, {3, 61, 2, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {51, 3, 45}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_added" &&
               call(WseOpcode::dict_get_str, {51, 3, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_old",
           "dict_load_file(mode=2, format=1) inserts missing keys from .ini");
    expect(call(WseOpcode::dict_load_file, {3, 61, 1, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {51, 3, 42}) ==
                   WseDispatch::handled_no_store &&
               str_regs[51] == u"v_new",
           "dict_load_file(mode=1, format=1) overwrites keys from .ini");
    // Corrupt/truncated .wsedict does not crash.
    {
        std::ofstream corrupt(temp_dir / "corrupt.wsedict", std::ios::binary);
        const char bytes[] = {'\x7f', '\x00', '\x00', '\x00', '\x01', '\x00'};
        corrupt.write(bytes, sizeof(bytes));
    }
    str_regs[62] = u"corrupt";
    expect(call(WseOpcode::dict_load_file, {3, 62, 1}) ==
               WseDispatch::handled_no_store,
           "dict_load_file rejects truncated/malformed binary safely");
    expect(call(WseOpcode::dict_delete_file, {60}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "bin_dict.wsedict") &&
               call(WseOpcode::dict_delete_file, {61, 1}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "ini_dict.ini"),
           "dict_delete_file deletes .wsedict and .ini targets");

    // 3217/3218/3222 JSON save/load/delete including \uXXXX and surrogate pairs.
    {
        std::ofstream js(temp_dir / "unicode.json", std::ios::binary);
        js << "{\"uml\":\"\\u00e9\",\"smile\":\"\\uD83D\\uDE00\",\"esc\":\"a\\\\b\\nc\"}";
    }
    str_regs[63] = u"unicode";
    str_regs[64] = u"uml";
    str_regs[65] = u"smile";
    str_regs[66] = u"esc";
    expect(call(WseOpcode::dict_load_file_json, {3, 63, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {52, 3, 64}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"\u00e9" &&
               call(WseOpcode::dict_get_str, {52, 3, 65}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"\U0001F600" &&
               call(WseOpcode::dict_get_str, {52, 3, 66}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"a\\b\nc",
           "dict_load_file_json decodes \\uXXXX, UTF-16 surrogates, and escapes");
    str_regs[67] = u"roundtrip_json";
    expect(call(WseOpcode::dict_save_json, {3, 67}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "roundtrip_json.json") &&
               call(WseOpcode::dict_clear, {3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_load_file_json, {3, 67, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {52, 3, 65}) ==
                   WseDispatch::handled_no_store &&
               str_regs[52] == u"\U0001F600" &&
               call(WseOpcode::dict_delete_file_json, {67}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "roundtrip_json.json"),
           "dict_save_json / load / delete round-trip cleanly");

    // 3219 dict_from_url_encoded_json & 3220 dict_to_url_encoded_json.
    expect(call(WseOpcode::dict_clear, {3}) == WseDispatch::handled_no_store,
           "cleared dict 3");
    str_regs[70] = u"a+b";
    str_regs[71] = u"hello world!";
    expect(call(WseOpcode::dict_set_str, {3, 70, 71}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_to_url_encoded_json, {80, 3}) ==
                   WseDispatch::handled_no_store &&
               str_regs[80] == u"%7B%22a%2Bb%22%3A%22hello%20world%21%22%7D",
           "dict_to_url_encoded_json percent-encodes reserved bytes and + as %2B");
    // Now verify dict_from_url_encoded_json turns '+' into space while keeping %2B as '+'.
    str_regs[81] = u"%7B%22a%2Bb%22%3A+%22hello+world%21%22%7D";
    expect(call(WseOpcode::dict_set_str, {2, 40, 41}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_from_url_encoded_json, {2, 81, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_size, {0, 2}) == WseDispatch::handled &&
               effect.value == 1 &&
               call(WseOpcode::dict_get_str, {53, 2, 70}) ==
                   WseDispatch::handled_no_store &&
               str_regs[53] == u"hello world!",
           "dict_from_url_encoded_json converts + to space before unescape");
    // Mode 2 keeps existing entries when loading URL-encoded JSON.
    str_regs[82] = u"%7B%22a%2Bb%22%3A%22changed%22%2C%22extra%22%3A%22ok%22%7D";
    str_regs[83] = u"extra";
    expect(call(WseOpcode::dict_from_url_encoded_json, {2, 82, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::dict_get_str, {53, 2, 70}) ==
                   WseDispatch::handled_no_store &&
               str_regs[53] == u"hello world!" &&
               call(WseOpcode::dict_get_str, {53, 2, 83}) ==
                   WseDispatch::handled_no_store &&
               str_regs[53] == u"ok",
           "dict_from_url_encoded_json mode 2 inserts only missing keys");

    // mbCheckPath sandboxing rejects traversal, separators, wildcards, empty names.
    const std::u16string_view bad_paths[] = {
        u"../wse2_dict_escape_forbidden",
        u"sub/file",
        u"sub\\file",
        u"C:evil",
        u"bad?name",
        u"...",
        u"",
    };
    for (const auto bad : bad_paths) {
        str_regs[90] = std::u16string(bad);
        expect(call(WseOpcode::dict_save, {2, 90}) ==
                       WseDispatch::handled_no_store &&
                   call(WseOpcode::dict_save_json, {2, 90}) ==
                       WseDispatch::handled_no_store &&
                   !fs::exists(outside_escape),
               "mbCheckPath blocks unsafe/traversal file names");
    }

    // Invalid registers & operand count checks across dictionary ops.
    expect(call(WseOpcode::dict_get_str, {128, 2, 70}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_get_key_by_iterator, {-1, 2, 0}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_to_url_encoded_json, {128, 2}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_get_pos, {128, 0, 30}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_get_pos, {1, 0, 30, 128}) ==
                   WseDispatch::ignored &&
               call(WseOpcode::dict_set_pos, {0, 30, 128}) ==
                   WseDispatch::ignored,
           "dictionary ops return ignored for invalid register indices");
    expect(call(WseOpcode::dict_create, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_free, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_load_file, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_load_dict, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_save, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_clear, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_is_empty, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_has_key, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_size, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_delete_file, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_str, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_int, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_set_str, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_set_int, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_key_by_iterator, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_get_pos, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_set_pos, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_load_file_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_save_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_from_url_encoded_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_to_url_encoded_json, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_erase, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::dict_delete_file_json, {}) ==
                   WseDispatch::operand_count_out_of_range,
           "all 23 dictionary opcodes validate minimum operand count");
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

void test_feval() {
    std::array<std::u16string, 128> str_regs{};
    std::array<std::u16string_view, 2> global_strs = {u"fp5 = 99", u"bad"};
    std::array<std::u16string_view, 2> local_strs = {u"fp6 = 77", u"zero"};
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};
    for (auto& pos : positions) {
        pos = wse::vm::WseTransform::identity();
    }

    WseVmState state;
    state.strings.registers = str_regs;
    state.strings.global = global_strs;
    state.strings.local = local_strs;
    state.positions = positions;

    WseEffect effect{};
    const auto eval_expr = [&](std::u16string_view expr) {
        str_regs[0].assign(expr);
        WseOperands operands;
        const std::array<std::int64_t, 1> wide = {0};
        const std::array<std::int32_t, 1> narrow = {0};
        const std::array<std::uint8_t, 1> tags = {0};
        operands.wide = wide;
        operands.narrow = narrow;
        operands.tags = tags;
        operands.fp = fp;
        operands.registers = int_regs;
        operands.state = state;
        effect = WseEffect{0x1234, true};
        return wse::vm::execute_wse(opcode(WseOpcode::feval), operands, effect);
    };

    // Constants, unary precedence (-2^2 == -4), right-assoc ^, and assignment.
    // Retail mbBasicGame::parseString (0x140072338) turns literal '^' into
    // '\n', so caret power expressions arrive via un-reparsed {sN} tokens.
    str_regs[3] = u"-2^2";
    str_regs[4] = u"(-2)^2";
    str_regs[5] = u"2^3^2";
    str_regs[6] = u"2^-2";
    expect(eval_expr(u"fp0 = _pi, fp1 = _e, fp2 = {s3}, fp3 = {s4}, fp4 = {s5}") ==
                   WseDispatch::handled_no_store &&
               close(fp[0], 3.1415927f) && close(fp[1], 2.7182817f) &&
               close(fp[2], -4.0f) && close(fp[3], 4.0f) && close(fp[4], 512.0f),
           "feval evaluates constants, -2^2=-4 precedence, and right-assoc power");

    // Global (tag 3) and local (tag 0x16) string register dispatch, plus invalid.
    expect(run(opcode(WseOpcode::feval), {0}, {0}, effect, fp, int_regs, state, {3}) ==
                   WseDispatch::handled_no_store &&
               close(fp[5], 99.0f) &&
               run(opcode(WseOpcode::feval), {0}, {0}, effect, fp, int_regs, state, {0x16}) ==
                   WseDispatch::handled_no_store &&
               close(fp[6], 77.0f) &&
               run(opcode(WseOpcode::feval), {2}, {2}, effect, fp, int_regs, state, {3}) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::feval), {2}, {2}, effect, fp, int_regs, state, {0x16}) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::feval), {128}, {128}, effect, fp, int_regs, state, {0}) ==
                   WseDispatch::ignored,
           "feval supports tag 0/3/0x16 string operands and rejects out-of-range");

    // parseString expansion inside feval: {regN}, {regN?a:b}, {sN} non-recursive, {!/***}.
    int_regs[0] = -7;
    int_regs[1] = 1;
    str_regs[1] = u"10 + 5";
    str_regs[2] = u"{reg0}";  // Should NOT be re-parsed when expanded via {s2}.
    expect(eval_expr(u"fp10 = {reg0}{!ignored}{***also ignored}, fp11 = ({s1}) * ({reg1?2:3}), fp12 = {reg0?{reg1?4:5}:9}") ==
                   WseDispatch::handled_no_store &&
               close(fp[10], -7.0f) && close(fp[11], 30.0f) && close(fp[12], 4.0f),
           "feval expands {regN}, {regN?a:b}, {sN}, and strips comment tokens");

    // Non-recursive {s2} containing "{reg0}" should fail mu::Parser cleanly without mutating fp13.
    fp[13] = 42.0f;
    expect(eval_expr(u"fp13 = 1, fp14 = {s2}") == WseDispatch::handled_no_store &&
               close(fp[13], 42.0f),
           "parseString does not recursively expand braces inside s[n]");

    // Position coordinates (all 12 components: sx..sz, fx..fz, ux..uz, x..z).
    expect(eval_expr(u"pos0sx=1, pos0sy=2, pos0sz=3, pos1fx=4, pos1fy=5, pos1fz=6, pos2ux=7, pos2uy=8, pos2uz=9, pos3x=10, pos3y=11, pos3z=12") ==
                   WseDispatch::handled_no_store &&
               close(positions[0].s.x, 1.0f) && close(positions[0].s.y, 2.0f) &&
               close(positions[0].s.z, 3.0f) && close(positions[1].f.x, 4.0f) &&
               close(positions[1].f.y, 5.0f) && close(positions[1].f.z, 6.0f) &&
               close(positions[2].u.x, 7.0f) && close(positions[2].u.y, 8.0f) &&
               close(positions[2].u.z, 9.0f) && close(positions[3].o.x, 10.0f) &&
               close(positions[3].o.y, 11.0f) && close(positions[3].o.z, 12.0f),
           "feval reads and mutates all 12 pos<N><comp> fields");

    // Ternary short-circuiting & logical/relational operators (plus literal '^' -> '\n').
    fp[20] = 100.0f;
    fp[21] = 200.0f;
    fp[24] = 9.0f;
    expect(eval_expr(u"fp22 = (1 < 2 && 3 >= 3 && 4 != 5 && 0 == 0) ? (fp20 = 11) : (fp21 = 99), fp23 = (0 || 0) ? 1 : {s6}") ==
                   WseDispatch::handled_no_store &&
               eval_expr(u"fp24 = 2^3") == WseDispatch::handled_no_store &&
               close(fp[20], 11.0f) && close(fp[21], 200.0f) &&
               close(fp[22], 11.0f) && close(fp[23], 0.25f) &&
               close(fp[24], 9.0f),
           "feval evaluates logical, comparison, negative exponent, and short-circuit ternary");

    // Math functions: deg2rad, rad2deg, clamp(lo>hi), rint, sign, ln/log/log2/log10, fmod, atan2, sum/avg/min/max.
    expect(eval_expr(u"fp30 = deg2rad(180), fp31 = rad2deg(fp30), fp32 = clamp(5, 10, 2), fp33 = clamp(15, 10, 2), fp34 = rint(2.5), fp35 = sign(-9) + sign(0) + sign(4)") ==
                   WseDispatch::handled_no_store &&
               close(fp[30], 3.1415927f) && close(fp[31], 180.0f, 1e-4f) &&
               close(fp[32], 5.0f) && close(fp[33], 10.0f) &&
               close(fp[34], 3.0f) && close(fp[35], 0.0f) &&
               eval_expr(u"fp36 = ln(_e) + log(_e) + log2(8) + log10(1000) + abs(-4) + sqrt(9) + floor(2.9) + ceil(2.1) + fmod(7.5, 2) + round_missing_noop = 0") ==
                   WseDispatch::handled_no_store,
           "feval validates bad syntax before mutating");
    expect(close(fp[36], 0.0f) &&
               eval_expr(u"fp36 = ln(_e) + log(_e) + log2(8) + log10(1000) + abs(-4) + sqrt(9) + floor(2.9) + ceil(2.1) + fmod(7.5, 2), fp37 = atan2(0, -1), fp38 = sum(1,2,3,4), fp39 = avg(2,4,6), fp40 = min(9,2,5) + max(9,2,5), fp41 = sin(0) + cos(0) + tan(0) + exp(0) + sinh(0) + cosh(0) + tanh(0) + asinh(0) + acosh(1) + atanh(0) + asin(0) + acos(1) + atan(0)") ==
                   WseDispatch::handled_no_store &&
               close(fp[36], 1.0f + 1.0f + 3.0f + 3.0f + 4.0f + 3.0f + 2.0f + 3.0f + 1.5f) &&
               close(fp[37], 3.1415927f, 1e-4f) &&
               close(fp[38], 10.0f) && close(fp[39], 4.0f) &&
               close(fp[40], 11.0f) && close(fp[41], 3.0f),
           "feval evaluates all built-in math and variadic functions");

    // Atomic error non-mutation checks: non-lvalue assignment, unknown vars, bad arity.
    fp[50] = 12.5f;
    positions[10].o.x = 3.5f;
    expect(eval_expr(u"fp50 = 1, 2 = 3") == WseDispatch::handled_no_store &&
               eval_expr(u"fp50 = 99, pos0 = 1") == WseDispatch::handled_no_store &&
               eval_expr(u"fp50 = 99, sum()") == WseDispatch::handled_no_store &&
               eval_expr(u"fp50 = 99, clamp(1, 2)") == WseDispatch::handled_no_store &&
               eval_expr(u"pos10x = 9, fp128 = 1") == WseDispatch::handled_no_store &&
               close(fp[50], 12.5f) && close(positions[10].o.x, 3.5f) &&
               run(opcode(WseOpcode::feval), {}, {}, effect) ==
                   WseDispatch::operand_count_out_of_range,
           "feval aborts cleanly on syntax/lvalue/arity errors without mutation");
}

void test_array_group() {
    namespace fs = std::filesystem;
    const fs::path temp_dir =
        fs::temp_directory_path() / "wse2_array_unit_test_tmp";
    const fs::path outside_escape =
        fs::temp_directory_path() / "wse2_array_escape_forbidden.wsearray";
    struct TempGuard {
        fs::path dir;
        fs::path escape;
        ~TempGuard() {
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::remove(escape, ec);
        }
    } guard{temp_dir, outside_escape};
    {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::remove(outside_escape, ec);
        fs::create_directories(temp_dir, ec);
        expect(!ec, "created temporary directory for array file tests");
    }

    std::array<std::u16string, 128> str_regs{};
    std::array<float, wse::vm::kFpRegisterCount> fp{};
    std::array<std::int64_t, 128> int_regs{};
    std::array<wse::vm::WseTransform, 128> positions{};
    for (auto& pos : positions) {
        pos = wse::vm::WseTransform::identity();
    }
    wse::vm::WseObjectManager objects;
    objects.root_dir = temp_dir;

    WseVmState state;
    state.strings.registers = str_regs;
    state.positions = positions;
    state.dictionaries = &objects;

    WseEffect effect{};
    const auto call = [&](WseOpcode op,
                          std::initializer_list<std::int64_t> wide_args,
                          std::initializer_list<std::uint8_t> tag_args = {}) {
        std::vector<std::int64_t> wide(wide_args);
        std::vector<std::int32_t> narrow(wide.size());
        for (std::size_t i = 0; i < wide.size(); ++i) {
            narrow[i] = static_cast<std::int32_t>(wide[i]);
        }
        std::vector<std::uint8_t> tags(tag_args);
        WseOperands operands;
        operands.wide = wide;
        operands.narrow = narrow;
        operands.tags = tags;
        operands.fp = fp;
        operands.registers = int_regs;
        operands.state = state;
        effect = WseEffect{0x1234, true};
        return wse::vm::execute_wse(opcode(op), operands, effect);
    };

    // Unified WseObjectManager ID sharing and cross-type rejection.
    expect(call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 0,
           "unified pool allocates dict id 0");
    expect(call(WseOpcode::array_create, {0, 0, 3}) == WseDispatch::handled &&
               effect.value == 1,
           "unified pool allocates array id 1 next");
    expect(call(WseOpcode::dict_get_size, {0, 1}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_get_type_id, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_free, {0}) == WseDispatch::handled_no_store,
           "cross-type dictionary/array operations safely reject mismatched handles");
    expect(call(WseOpcode::array_free, {1}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_create, {0}) == WseDispatch::handled &&
               effect.value == 1,
           "dict_create reuses freed array handle in LIFO order");
    expect(call(WseOpcode::dict_free, {0}) == WseDispatch::handled_no_store &&
               call(WseOpcode::dict_free, {1}) == WseDispatch::handled_no_store,
           "cleaned up dict handles");

    // 5000 array_create validation + 1..14D + default initialization.
    expect(call(WseOpcode::array_create, {0, 3, 2}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_create, {0, 0, -1}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_create, {0, 0, 2, 0}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_create,
                    {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}) ==
                   WseDispatch::handled &&
               effect.value == 0,
           "array_create stores 0 on invalid type_id, dims, or >14 dimensions");

    expect(call(WseOpcode::array_create,
                {0, 0, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}) ==
               WseDispatch::handled,
           "array_create creates 14D integer array");
    const std::int64_t arr14d = effect.value;
    expect(call(WseOpcode::array_get_dim_count, {0, arr14d}) == WseDispatch::handled &&
               effect.value == 14 &&
               call(WseOpcode::array_get_dim_size, {0, arr14d, 0}) ==
                   WseDispatch::handled &&
               effect.value == 2 &&
               call(WseOpcode::array_get_dim_size, {0, arr14d, 14}) ==
                   WseDispatch::handled &&
               effect.value == -1 &&
               call(WseOpcode::array_get_val,
                    {0, arr14d, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}) ==
                   WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_free, {arr14d}) == WseDispatch::handled_no_store,
           "14D array reports dim count/sizes, default 0 values, and -1 OOB dim");

    // 1D int array: set_val, set_val_all, resize_dim (size>=1), copy, eq..le.
    expect(call(WseOpcode::array_create, {0, 0, 3}) == WseDispatch::handled,
           "create 1D int array");
    const std::int64_t ints_id = effect.value;
    expect(call(WseOpcode::array_set_val_all, {ints_id, 42}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_set_val, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_resize_dim, {ints_id, 0, 5}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_resize_dim, {ints_id, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_dim_size, {0, ints_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5 &&
               call(WseOpcode::array_get_val, {0, ints_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 42 &&
               call(WseOpcode::array_get_val, {0, ints_id, 1}) ==
                   WseDispatch::handled &&
               effect.value == 15 &&
               call(WseOpcode::array_get_val, {0, ints_id, 4}) ==
                   WseDispatch::handled &&
               effect.value == 0,
           "int array set_val_all, set_val, and resize_dim preserve overlap and zero-fill");

    expect(call(WseOpcode::array_eq, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_neq, {ints_id, 99, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_gt, {ints_id, 10, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_ge, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_lt, {ints_id, 20, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_le, {ints_id, 15, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_gt, {ints_id, 15, 1}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::array_eq, {ints_id, 15, 99}) ==
                   WseDispatch::condition_false,
           "array_eq..array_le evaluate integer comparisons and OOB indices");

    // 5002 array_copy creates independent deep copy; bad handle does not store.
    expect(call(WseOpcode::array_copy, {0, ints_id}) == WseDispatch::handled,
           "array_copy duplicates int array");
    const std::int64_t copy_id = effect.value;
    expect(call(WseOpcode::array_set_val, {copy_id, 999, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {0, ints_id, 1}) ==
                   WseDispatch::handled &&
               effect.value == 15 &&
               call(WseOpcode::array_copy, {0, 9999}) ==
                   WseDispatch::handled_no_store &&
               effect.value == 0x1234,
           "array_copy is deep and ignores invalid source ID");
    (void)call(WseOpcode::array_free, {copy_id});

    // 5009 / 5010 push & pop on 1D int/string/pos + 2D multidim + empty pop guard.
    expect(call(WseOpcode::array_push, {ints_id, 77}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_pop, {0, ints_id}) == WseDispatch::handled &&
               effect.value == 77 &&
               call(WseOpcode::array_get_dim_size, {0, ints_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5,
           "1D int array_push/array_pop updates element and dimension size");

    expect(call(WseOpcode::array_create, {0, 1, 0}) == WseDispatch::handled,
           "create 1D empty string array");
    const std::int64_t strs_id = effect.value;
    int_regs[3] = 55;
    str_regs[0] = u"val^{reg3}";
    str_regs[1] = u"Alpha";
    str_regs[2] = u"alpha";
    str_regs[3] = u"beta";
    expect(call(WseOpcode::array_push, {strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_push, {strs_id, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_push, {strs_id, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_push, {strs_id, 3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {5, strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               str_regs[5] == u"val\n55" &&
               call(WseOpcode::array_pop, {128, strs_id}) == WseDispatch::ignored &&
               call(WseOpcode::array_get_dim_size, {0, strs_id, 0}) ==
                   WseDispatch::handled &&
               effect.value == 4 &&
               call(WseOpcode::array_lt, {strs_id, 2, 1}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_pop, {6, strs_id}) ==
                   WseDispatch::handled_no_store &&
               str_regs[6] == u"beta",
           "string array expands parseString, guards pop dst register, and compares bytes");

    // 5015 array_sort: CS/CI string sort + 2D stability check!
    expect(call(WseOpcode::array_sort, {strs_id, 2}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {10, strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {11, strs_id, 1}) ==
                   WseDispatch::handled_no_store &&
               str_regs[10] == u"Alpha" && str_regs[11] == u"alpha" &&
               call(WseOpcode::array_sort, {strs_id, 3}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {10, strs_id, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {11, strs_id, 1}) ==
                   WseDispatch::handled_no_store &&
               str_regs[10] == u"val\n55" && str_regs[11] == u"Alpha",
           "string array_sort supports CI ascending/descending while staying stable");

    // 2D int array stability, subdim sort, push_subarray, pop_subarray!
    expect(call(WseOpcode::array_create, {0, 0, 4, 2}) == WseDispatch::handled,
           "create 2D (4x2) int array");
    const std::int64_t arr2d = effect.value;
    // Rows: (key=20, id=0), (key=10, id=1), (key=20, id=2), (key=10, id=3).
    (void)call(WseOpcode::array_set_val, {arr2d, 20, 0, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 0, 0, 1});
    (void)call(WseOpcode::array_set_val, {arr2d, 10, 1, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 1, 1, 1});
    (void)call(WseOpcode::array_set_val, {arr2d, 20, 2, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 2, 2, 1});
    (void)call(WseOpcode::array_set_val, {arr2d, 10, 3, 0});
    (void)call(WseOpcode::array_set_val, {arr2d, 3, 3, 1});
    expect(call(WseOpcode::array_sort, {arr2d, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_val, {0, arr2d, 0, 1}) ==
                   WseDispatch::handled &&
               effect.value == 1 &&
               call(WseOpcode::array_get_val, {0, arr2d, 1, 1}) ==
                   WseDispatch::handled &&
               effect.value == 3 &&
               call(WseOpcode::array_get_val, {0, arr2d, 2, 1}) ==
                   WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_get_val, {0, arr2d, 3, 1}) ==
                   WseDispatch::handled &&
               effect.value == 2,
           "2D array_sort permutes dim 0 stably by key at subdims");

    // 1D subarray push + pop on 2D array + mismatched push rejection.
    expect(call(WseOpcode::array_create, {0, 0, 2}) == WseDispatch::handled,
           "create matching 1D slice");
    const std::int64_t row_id = effect.value;
    (void)call(WseOpcode::array_set_val, {row_id, 50, 0});
    (void)call(WseOpcode::array_set_val, {row_id, 99, 1});
    expect(call(WseOpcode::array_push, {arr2d, row_id}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_dim_size, {0, arr2d, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5 &&
               call(WseOpcode::array_push, {arr2d, ints_id}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_get_dim_size, {0, arr2d, 0}) ==
                   WseDispatch::handled &&
               effect.value == 5 &&
               call(WseOpcode::array_pop, {0, arr2d}) == WseDispatch::handled,
           "multidim push/pop rejects mismatched dims and creates subarray on pop");
    const std::int64_t popped_row = effect.value;
    expect(call(WseOpcode::array_get_dim_count, {0, popped_row}) ==
                   WseDispatch::handled &&
               effect.value == 1 &&
               call(WseOpcode::array_get_val, {0, popped_row, 1}) ==
                   WseDispatch::handled &&
               effect.value == 99,
           "multidim popped row contains pushed elements");

    // Position arrays: default zeroed transform, orthonormalize compare in array_eq/neq, file I/O!
    expect(call(WseOpcode::array_create, {0, 2, 2}) == WseDispatch::handled,
           "create 1D position array");
    const std::int64_t pos_arr = effect.value;
    expect(call(WseOpcode::array_get_val, {20, pos_arr, 0}) ==
                   WseDispatch::handled_no_store &&
               positions[20].s.x == 0.0f && positions[20].f.y == 0.0f &&
               positions[20].o.x == 0.0f,
           "position array default initializes to 0-position");
    positions[1] = wse::vm::WseTransform::identity();
    positions[1].o = {1.5f, -2.25f, 3.75f, 1.0f};
    positions[1].f = {0.0f, 5.0f, 0.0f, 0.0f};  // Unnormalized forward, orthonormalize makes identical!
    positions[2] = wse::vm::WseTransform::identity();
    positions[2].o = {1.5f, -2.25f, 3.75f, 1.0f};
    positions[3] = wse::vm::WseTransform::identity();
    positions[3].o = {9.0f, 0.0f, 0.0f, 1.0f};
    expect(call(WseOpcode::array_set_val, {pos_arr, 1, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_eq, {pos_arr, 2, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_neq, {pos_arr, 3, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_gt, {pos_arr, 2, 0}) ==
                   WseDispatch::condition_false &&
               call(WseOpcode::array_sort, {pos_arr, 0}) ==
                   WseDispatch::handled_no_store,
           "position array_eq/neq orthonormalizes copies and rejects ordered compare ops");

    // .wsearray binary save / load / delete + path traversal & version > 1 checks!
    str_regs[0] = u"unit_pos_{reg3}";
    str_regs[1] = u"../wse2_array_escape_forbidden";
    expect(call(WseOpcode::array_save_file, {pos_arr, 0}) ==
                   WseDispatch::handled_no_store &&
               fs::exists(temp_dir / "unit_pos_55.wsearray") &&
               call(WseOpcode::array_save_file, {pos_arr, 1}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(outside_escape) &&
               call(WseOpcode::array_load_file, {0, 0}) == WseDispatch::handled &&
               effect.value > 0,
           "array_save_file/load_file roundtrip .wsearray and reject traversal paths");
    const std::int64_t loaded_pos = effect.value;
    expect(call(WseOpcode::array_eq, {loaded_pos, 2, 0}) ==
                   WseDispatch::handled_no_store,
           "loaded .wsearray orthonormalizes transforms cleanly");

    // Write a version=2 .wsearray header and confirm load returns 0.
    {
        const fs::path bad_ver = temp_dir / "future_ver.wsearray";
        std::ofstream out(bad_ver, std::ios::binary | std::ios::trunc);
        const std::array<std::uint8_t, 16> header = {
            0x41, 0x45, 0x53, 0x57,  // LE 0x57534541
            0x02, 0x00, 0x00, 0x00,  // version 2 (> 1)
            0x00, 0x00, 0x00, 0x00,  // int
            0x01, 0x00, 0x00, 0x00   // dimCount = 1
        };
        out.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
    }
    str_regs[4] = u"future_ver";
    expect(call(WseOpcode::array_load_file, {0, 4}) == WseDispatch::handled &&
               effect.value == 0 &&
               call(WseOpcode::array_delete_file, {0}) ==
                   WseDispatch::handled_no_store &&
               !fs::exists(temp_dir / "unit_pos_55.wsearray"),
           "array_load_file rejects version>1 with 0 and delete_file removes .wsearray");

    // Binary .wsearray save/load roundtrip for 2D integer + string arrays.
    str_regs[7] = u"arr2d_bin";
    str_regs[8] = u"strs_bin";
    expect(call(WseOpcode::array_save_file, {arr2d, 7}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_save_file, {strs_id, 8}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_load_file, {0, 7}) == WseDispatch::handled &&
               effect.value > 0 &&
               call(WseOpcode::array_eq, {effect.value, 10, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               call(WseOpcode::array_load_file, {0, 8}) == WseDispatch::handled &&
               effect.value > 0 &&
               call(WseOpcode::array_get_val, {12, effect.value, 0}) ==
                   WseDispatch::handled_no_store &&
               str_regs[12] == u"val\n55",
           ".wsearray binary save/load roundtrips 2D int and UTF-8 string arrays");

    // Operand-count checks across array ops.
    expect(call(WseOpcode::array_create, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_free, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_copy, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_save_file, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_load_file, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_delete_file, {}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_set_val, {0, 1}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_set_val_all, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_val, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_push, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_pop, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_resize_dim, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_dim_size, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_dim_count, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_get_type_id, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_sort, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_sort_custom, {0}) ==
                   WseDispatch::operand_count_out_of_range &&
               call(WseOpcode::array_eq, {0, 0}) ==
                   WseDispatch::operand_count_out_of_range,
           "array ops validate operand counts");
}

void test_slot_gt_group() {
    WseEffect effect;
    wse::game::WseWorldState world{};
    world.items.push_back(wse::game::WseItemKind{});
    world.items[0].slots.set(4, 15);
    world.party_templates.push_back(wse::game::WsePartyTemplate{});
    world.party_templates[0].slots.set(1, 20);
    world.troops.push_back(wse::game::WseTroop{});
    world.troops[0].slots.set(7, 50);
    world.factions.push_back(wse::game::WseFaction{});
    world.factions[0].slots.set(2, -3);
    world.quests.push_back(wse::game::WseQuest{});
    world.quests[0].slots.set(0, 9);
    world.sites.push_back(wse::game::WseSite{});
    world.sites[0].slots.set(3, 42);
    world.parties.resize(2);
    world.parties[0].active = true;
    world.parties[0].slots.set(5, 100);
    world.parties[1].active = false;
    world.parties[1].slots.set(5, 999);
    world.players.resize(2);
    world.players[0].status = 2;
    world.players[0].slots.set(6, 77);
    world.players[1].status = 1;
    world.players[1].slots.set(6, 999);
    world.teams[7].slots.set(8, 88);
    world.agents.resize(2);
    world.agents[0].active = true;
    world.agents[0].slots.set(9, 64);
    world.agents[1].active = false;
    world.agents[1].slots.set(9, 999);
    world.mission_objects.resize(2);
    world.mission_objects[0].active = true;
    world.mission_objects[0].slots.set(10, 33);
    world.mission_objects[1].active = false;
    world.mission_objects[1].slots.set(10, 999);

    WseVmState state{};
    state.world = &world;

    auto check_slot = [&](WseOpcode op, std::int64_t id, std::int64_t slot,
                          std::int64_t rhs) {
        return run(opcode(op), {id, slot, rhs},
                   {static_cast<std::int32_t>(id),
                    static_cast<std::int32_t>(slot),
                    static_cast<std::int32_t>(rhs)},
                   effect, {}, {}, state);
    };

    expect(check_slot(WseOpcode::item_slot_gt, 0, 4, 14) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::item_slot_gt, 0, 4, 15) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_template_slot_gt, 0, 1, 19) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::party_template_slot_gt, 0, 1, 20) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::troop_slot_gt, 0, 7, 49) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::troop_slot_gt, 0, 7, 50) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::faction_slot_gt, 0, 2, -4) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::faction_slot_gt, 0, 2, -3) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::quest_slot_gt, 0, 0, 8) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::quest_slot_gt, 0, 0, 9) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_slot_gt, 0, 3, 41) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::scene_slot_gt, 0, 3, 42) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_slot_gt, 0, 5, 99) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::party_slot_gt, 0, 5, 100) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::player_slot_gt, 0, 6, 76) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::player_slot_gt, 0, 6, 77) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::team_slot_gt, 7, 8, 87) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::team_slot_gt, 7, 8, 88) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::agent_slot_gt, 0, 9, 63) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::agent_slot_gt, 0, 9, 64) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_prop_slot_gt, 0, 10, 32) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::scene_prop_slot_gt, 0, 10, 33) ==
                   WseDispatch::condition_false,
           "all 11 *_slot_gt opcodes evaluate true and false boundaries");

    // Unset valid slots read 0; out-of-range slot indices and inactive entities fail.
    expect(check_slot(WseOpcode::item_slot_gt, 0, 0xfffff, -5) ==
                   WseDispatch::handled_no_store &&
               check_slot(WseOpcode::item_slot_gt, 0, 0xfffff, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::item_slot_gt, 0, -1, -5) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::item_slot_gt, 0, 0x100000, -5) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_slot_gt, 1, 5, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::player_slot_gt, 1, 6, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::agent_slot_gt, 1, 9, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_prop_slot_gt, 1, 10, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::team_slot_gt, 8, 0, -5) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::troop_slot_gt, -1, 0, -5) ==
                   WseDispatch::condition_false &&
               run(opcode(WseOpcode::troop_slot_gt), {0, 0}, {0, 0}, effect, {},
                   {}, state) == WseDispatch::operand_count_out_of_range,
           "*_slot_gt enforces slot range 0..0xfffff, entity activity, and operand count");

    world.has_game = false;
    expect(check_slot(WseOpcode::troop_slot_gt, 0, 7, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::faction_slot_gt, 0, 2, -10) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::quest_slot_gt, 0, 0, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::scene_slot_gt, 0, 3, 0) ==
                   WseDispatch::condition_false &&
               check_slot(WseOpcode::party_slot_gt, 0, 5, 0) ==
                   WseDispatch::condition_false,
           "game-scoped *_slot_gt handlers fail cleanly when !has_game");
}

void test_scalar_config_skill_savegame_group() {
    WseEffect effect;
    wse::vm::WseOptionsState options{};
    std::array<std::u16string, 128> registers{};
    wse::game::WseWorldState world{};
    world.parties.resize(3);
    world.parties[0].active = true;
    world.parties[1].active = false;
    world.parties[2].active = true;
    world.skills[4].id = "skl_trade";
    world.skills[4].name = "Trade";
    world.skills[4].description = "Base trade description.";
    world.skills[4].flags = wse::game::kSkillPartyFlagBit;
    world.skills[5].id = "skl_ironflesh";
    world.skills[5].name = "Ironflesh";
    world.skills[5].description = "Base ironflesh description.";
    world.skills[5].flags = 0u;
    world.translations["skl_trade_desc"] = "Translated trade description.";
    world.module_settings["net_config"]["max_players"] = "64";
    world.game_variables["hero_bonus"] = "125";

    const auto save_dir =
        std::filesystem::temp_directory_path() / "wse_test_savegame_md5_dir";
    std::error_code ec;
    std::filesystem::remove_all(save_dir, ec);
    std::filesystem::create_directories(save_dir, ec);
    {
        std::ofstream empty_sav(save_dir / "sg00.sav", std::ios::binary);
    }
    {
        std::ofstream abc_sav(save_dir / "sg09.sav", std::ios::binary);
        abc_sav.write("abc", 3);
    }
    world.savegames_dir = save_dir;

    WseVmState state{};
    state.options = &options;
    state.world = &world;
    state.strings = {registers, {}, {}};

    // sleep_ms (3026).
    expect(run(opcode(WseOpcode::sleep_ms), {-1}, {-1}, effect, {}, {}, state) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::sleep_ms), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::sleep_ms), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store,
           "sleep_ms skips negative durations and handles 0/1 ms");

    // set_main_party / get_main_party (3030 / 3031).
    expect(run(opcode(WseOpcode::set_main_party), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::ignored &&
               run(opcode(WseOpcode::set_main_party), {2}, {2}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::get_main_party), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
               effect.value == 2 && effect.store_value,
           "set_main_party validates active parties and get_main_party reads back");

    // is_party_skill (3036).
    expect(run(opcode(WseOpcode::is_party_skill), {4}, {4}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::is_party_skill), {5}, {5}, effect, {}, {}, state) ==
                   WseDispatch::condition_false &&
               run(opcode(WseOpcode::is_party_skill), {42}, {42}, effect, {}, {}, state) ==
                   WseDispatch::ignored,
           "is_party_skill checks bit 4 across 0..41");

    // campaign_time (3037 / 3038) & show_hud (3045).
    expect(run(opcode(WseOpcode::set_campaign_time), {0x123456789abcLL}, {0},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::get_campaign_time), {0}, {0}, effect, {}, {},
                   state) == WseDispatch::handled &&
               effect.value == 0x123456789abcLL &&
               run(opcode(WseOpcode::show_hud), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.hide_hud &&
               run(opcode(WseOpcode::show_hud), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               !world.hide_hud,
           "campaign_time keeps 64-bit values and show_hud inverts into hide_hud");

    // set_show_use_tooltip (3601), stop_time (3608), get_water_level (3610).
    world.water_level = 2.506f;
    state.fixed_point_multiplier = 100;
    expect(run(opcode(WseOpcode::set_show_use_tooltip), {3, 1}, {3, 1}, effect,
               {}, {}, state) == WseDispatch::handled_no_store &&
               world.use_objects_tooltip_enabled[3] &&
               run(opcode(WseOpcode::set_show_use_tooltip), {3}, {3}, effect, {},
                   {}, state) == WseDispatch::handled_no_store &&
               !world.use_objects_tooltip_enabled[3] &&
               run(opcode(WseOpcode::set_show_use_tooltip), {11, 1}, {11, 1},
                   effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::stop_time), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::ignored &&
               !world.paused,
           "set_show_use_tooltip validates 0..10 and stop_time requires cheat_mode");

    options.cheat_mode = true;
    expect(run(opcode(WseOpcode::stop_time), {1}, {1}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.paused &&
               run(opcode(WseOpcode::stop_time), {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               !world.paused &&
               run(opcode(WseOpcode::get_water_level), {0}, {0}, effect, {}, {},
                   state) == WseDispatch::handled &&
               effect.value == 251,
           "stop_time toggles m_paused in cheat_mode and get_water_level rounds via rglRound64");

    // 3619 / 3622 options flags.
    expect(!options.horse_friendly_fire && options.show_crosshair &&
               run(opcode(WseOpcode::set_horse_friendly_fire), {1}, {1}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               options.horse_friendly_fire &&
               run(opcode(WseOpcode::set_show_crosshair), {0}, {0}, effect, {},
                   {}, state) == WseDispatch::handled_no_store &&
               !options.show_crosshair,
           "set_horse_friendly_fire and set_show_crosshair match retail defaults and toggles");

    // 4226 / 4231 module setting & game variable with {sN} parse + spacesToUnderscores.
    registers[1] = u"max {s3}";
    registers[2] = u"net config";
    registers[3] = u"players";
    registers[4] = u"Max Players";
    registers[5] = u"hero bonus";
    expect(run(opcode(WseOpcode::str_store_module_setting), {0, 1, 2},
               {0, 1, 2}, effect, {}, {}, state, {0, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               registers[0] == u"64" &&
               run(opcode(WseOpcode::str_store_module_setting), {0, 4, 2},
                   {0, 4, 2}, effect, {}, {}, state, {0, 0, 0}) ==
                   WseDispatch::handled_no_store &&
               registers[0].empty() &&
               run(opcode(WseOpcode::str_store_game_variable), {0, 5}, {0, 5},
                   effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               registers[0] == u"125",
           "str_store_module_setting and str_store_game_variable normalize spaces case-sensitively");

    // 4232 / 4245 str_store_skill_name / str_store_skill_desc.
    expect(run(opcode(WseOpcode::str_store_skill_name), {0, 4}, {0, 4}, effect,
               {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"Trade" &&
               run(opcode(WseOpcode::str_store_skill_desc), {0, 4}, {0, 4}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"Translated trade description." &&
               run(opcode(WseOpcode::str_store_skill_desc), {0, 5}, {0, 5}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"Base ironflesh description." &&
               run(opcode(WseOpcode::str_store_skill_name), {0, 42}, {0, 42},
                   effect, {}, {}, state) == WseDispatch::ignored,
           "str_store_skill_{name,desc} support _desc translations and fallback text");

    // 4246 str_store_savegame_md5.
    registers[0] = u"stale";
    expect(run(opcode(WseOpcode::str_store_savegame_md5), {0, 12}, {0, 12},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0].empty(),
           "str_store_savegame_md5 clears destination when savegame is missing");
    registers[0] = u"stale";
    expect(run(opcode(WseOpcode::str_store_savegame_md5), {0, 0}, {0, 0},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0].empty() &&
               run(opcode(WseOpcode::str_store_savegame_md5), {0, 9}, {0, 9},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"900150983cd24fb0d6963f7d28e17f72",
           "str_store_savegame_md5 skips empty files and formats sg%d%d.sav MD5");

    std::filesystem::remove_all(save_dir, ec);
}

void test_troop_group() {
    WseEffect effect;
    wse::game::WseWorldState world{};
    world.skills[wse::game::kSkillIronflesh].max_level = 10;
    world.skills[5].max_level = 6;
    world.skills[7].max_level = 10;
    world.skills[8].max_level = 10;
    world.skills[41].max_level = 10;

    world.troops.push_back(wse::game::WseTroop{});
    auto& troop = world.troops[0];
    troop.flags = 0x14ULL;
    troop.attributes[0] = 10;
    troop.hit_points = 45.0f;

    WseVmState state{};
    state.world = &world;

    // 3700..3705 points getters/setters.
    expect(run(opcode(WseOpcode::troop_set_skill_points), {0, 3}, {0, 3},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill_points), {0, -1}, {0, -1},
                   effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::troop_get_skill_points), {0, 0}, {0, 0},
                   effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 3 &&
               run(opcode(WseOpcode::troop_set_attribute_points), {0, 5},
                   {0, 5}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_get_attribute_points), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 5 &&
               run(opcode(WseOpcode::troop_set_proficiency_points), {0, 27},
                   {0, 27}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_get_proficiency_points), {0, 0},
                   {0, 0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 27,
           "troop points getters/setters preserve values on negative input");

    // 3706 troop_has_flag.
    expect(run(opcode(WseOpcode::troop_has_flag), {0, 0x04LL}, {0, 4}, effect,
               {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_has_flag), {0, 0x02LL}, {0, 2},
                   effect, {}, {}, state) == WseDispatch::condition_false &&
               run(opcode(WseOpcode::troop_has_flag), {9, 0x04LL}, {9, 4},
                   effect, {}, {}, state) == WseDispatch::ignored,
           "troop_has_flag tests flag bits and ignores invalid troop");

    // 3707 troop_set_skill (nibble packing, max_level clamp, Ironflesh HP + cap).
    expect(run(opcode(WseOpcode::troop_set_skill), {0, 5, 99}, {0, 5, 99},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill), {0, 7, 3}, {0, 7, 3},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill), {0, 8, 4}, {0, 8, 4},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::troop_set_skill), {0, 41, 5}, {0, 41, 5},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               troop.get_skill_raw(5) == 6 && troop.get_skill_raw(7) == 3 &&
               troop.get_skill_raw(8) == 4 && troop.get_skill_raw(41) == 5,
           "troop_set_skill packs 4-bit nibbles without clobbering neighbors");

    expect(run(opcode(WseOpcode::troop_set_skill),
               {0, wse::game::kSkillIronflesh, 4},
               {0, wse::game::kSkillIronflesh, 4}, effect, {}, {}, state) ==
               WseDispatch::handled_no_store &&
               close(troop.hit_points, 53.0f),
           "troop_set_skill adds 2 HP per gained Ironflesh level");

    troop.hit_points = 900.0f;
    expect(run(opcode(WseOpcode::troop_set_skill),
               {0, wse::game::kSkillIronflesh, 2},
               {0, wse::game::kSkillIronflesh, 2}, effect, {}, {}, state) ==
               WseDispatch::handled_no_store &&
               close(troop.hit_points, 49.0f),
           "troop_set_skill caps HP to str + ironflesh*2 + 35");

    // 3708 troop_set_attribute & 3709 troop_set_proficiency.
    expect(run(opcode(WseOpcode::troop_set_attribute), {0, 0, 2}, {0, 0, 2},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               troop.attributes[0] == 2 && close(troop.hit_points, 41.0f) &&
               run(opcode(WseOpcode::troop_set_attribute), {0, 1, -5},
                   {0, 1, -5}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               troop.attributes[1] == 0 &&
               run(opcode(WseOpcode::troop_set_proficiency), {0, 0, 1200},
                   {0, 0, 1200}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               close(troop.proficiencies[0], 999.0f) &&
               run(opcode(WseOpcode::troop_set_proficiency), {0, 1, -10},
                   {0, 1, -10}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               close(troop.proficiencies[1], 0.0f) &&
               run(opcode(WseOpcode::troop_set_proficiency), {0, 7, 100},
                   {0, 7, 100}, effect, {}, {}, state) == WseDispatch::ignored,
           "troop_set_attribute caps HP and troop_set_proficiency clamps to [0, 999]");
}

void test_item_group() {
    WseEffect effect;
    std::array<std::u16string, 128> registers{};
    registers[1] = u"iron plate";
    registers[2] = u"heavy {s1} mesh";
    registers[3] = u"metal armor";
    registers[4] = u"horse_skel";

    wse::game::WseWorldState world{};
    world.num_particle_systems = 10;
    world.items.push_back(wse::game::WseItemKind{});
    world.items[0].id = "itm_war_sword";
    world.items[0].variations = {{"war_sword_mesh", 0}};
    world.items[0].thrust_damage = (2 << 8) | 18;
    world.items[0].swing_damage = (0 << 8) | 30;

    wse::game::WseMetaMesh cur_meta{};
    wse::game::WseMesh lod0_mesh{};
    lod0_mesh.vertex_colors = {0xffffffffu, 0xffffffffu};
    cur_meta.lods = {{lod0_mesh}, {lod0_mesh, lod0_mesh}};
    world.cur_item_meta_mesh = &cur_meta;

    wse::game::WseMesh tmpl0{};
    tmpl0.name = "heavy0";
    tmpl0.vertex_colors = {0x10101010u};
    wse::game::WseMesh tmpl1{};
    tmpl1.name = "heavy1";
    tmpl1.vertex_colors = {0x20202020u};
    wse::game::WseMesh tmpl2{};
    tmpl2.name = "heavy2";
    tmpl2.vertex_colors = {0x30303030u};
    world.resource_meshes["heavy_iron_plate_mesh"] = {tmpl0, tmpl1, tmpl2};
    world.resource_materials["metal_armor"] = true;
    world.resource_skeleton_models["horse_skel"] = true;

    WseVmState state{};
    state.world = &world;
    state.strings = {registers, {}, {}};

    // 3800..3810 item setters.
    expect(run(opcode(WseOpcode::item_set_thrust_damage), {0, 300}, {0, 300},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.items[0].thrust_damage == ((2 << 8) | 44) &&
               run(opcode(WseOpcode::item_set_thrust_damage_type), {0, 5},
                   {0, 5}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.items[0].thrust_damage == ((1 << 8) | 44) &&
               run(opcode(WseOpcode::item_set_swing_damage), {0, 42}, {0, 42},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_swing_damage_type), {0, 2},
                   {0, 2}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.items[0].swing_damage == ((2 << 8) | 42),
           "item damage setters mask low byte and bits 8..9 independently");

    expect(run(opcode(WseOpcode::item_set_head_armor), {0, 11}, {0, 11},
               effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_body_armor), {0, 22}, {0, 22},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_leg_armor), {0, 33}, {0, 33},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_speed_rating), {0, 95}, {0, 95},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::item_set_missile_speed), {0, 75}, {0, 75},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.items[0].head_armor == 11 &&
               world.items[0].body_armor == 22 &&
               world.items[0].leg_armor == 33 &&
               world.items[0].speed_rating == 95 &&
               world.items[0].missile_speed == 75,
           "item armor and speed setters update fields");

    expect(run(opcode(WseOpcode::item_set_horse_blood_particles), {0, 2, 10},
               {0, 2, 10}, effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::item_set_horse_blood_particles), {0, 2, 9},
                   {0, 2, 9}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.items[0].blood_particle_system_nos[0] == 2 &&
               world.items[0].blood_particle_system_nos[1] == 9 &&
               run(opcode(WseOpcode::item_set_horse_blood_color),
                   {0, static_cast<std::int32_t>(0xffaabbccu)},
                   {0, static_cast<std::int32_t>(0xffaabbccu)}, effect, {}, {},
                   state) == WseDispatch::handled_no_store &&
               world.items[0].blood_color == 0xffaabbccu,
           "item_set_horse_blood_{particles,color} validate particle systems and store color");

    // 3811 cur_item_mesh_set_color & 3812 cur_item_add_mesh_with_material (caller == -50).
    expect(run(opcode(WseOpcode::cur_item_mesh_set_color), {0, 0x00112233},
               {0, 0x00112233}, effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::cur_item_add_mesh_with_material), {2, 3},
                   {2, 3}, effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::ignored,
           "cur_item_* reject calls outside caller == -50");

    state.caller = -50;
    expect(run(opcode(WseOpcode::cur_item_mesh_set_color), {1, 0x00445566},
               {1, 0x00445566}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               cur_meta.lods[0][0].vertex_colors[0] == 0xffffffffu &&
               cur_meta.lods[1][1].vertex_colors[0] == 0x00445566u &&
               cur_meta.lods[1][1].vertex_colors[1] == 0x00445566u,
           "cur_item_mesh_set_color colors matching mesh index across active LODs");

    expect(run(opcode(WseOpcode::cur_item_add_mesh_with_material), {2, 3},
               {2, 3}, effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               cur_meta.lods.size() == 2 && cur_meta.lods[0].size() == 2 &&
               cur_meta.lods[1].size() == 3 &&
               cur_meta.lods[0][1].material_name == "metal_armor" &&
               !cur_meta.lods[0][1].has_vertex_color_override &&
               run(opcode(WseOpcode::cur_item_add_mesh_with_material),
                   {2, 3, 2, 3, 0x00778899}, {2, 3, 2, 3, 0x00778899}, effect,
                   {}, {}, state, {0, 0}) == WseDispatch::handled_no_store &&
               cur_meta.lods.size() == 3 &&
               cur_meta.lods[2][0].material_name == "metal_armor" &&
               cur_meta.lods[2][0].has_vertex_color_override &&
               cur_meta.lods[2][0].vertex_color_override == 0x00778899u,
           "cur_item_add_mesh_with_material parses {sN}, normalizes spaces, and clones LODs");

    // 3813 item_set_horse_skeleton_model & 4235/4239 item string getters.
    registers[5] = u"missing_skel";
    expect(run(opcode(WseOpcode::item_set_horse_skeleton_model), {0, 5}, {0, 5},
               effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               world.items[0].skeleton_model.empty() &&
               run(opcode(WseOpcode::item_set_horse_skeleton_model), {0, 4},
                   {0, 4}, effect, {}, {}, state, {0, 0}) ==
                   WseDispatch::handled_no_store &&
               world.items[0].skeleton_model == "horse_skel" &&
               run(opcode(WseOpcode::str_store_item_id), {0, 0}, {0, 0}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"itm_war_sword" &&
               run(opcode(WseOpcode::str_store_item_mesh_name), {0, 0}, {0, 0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               registers[0] == u"war_sword_mesh",
           "item_set_horse_skeleton_model and str_store_item_{id,mesh_name} work cleanly");
}

void test_edit_mode_group() {
    WseEffect effect;
    wse::vm::WseOptionsState options{};
    wse::game::WseWorldState world{};
    world.mission_objects.resize(3);
    world.mission_objects[0].active = true;
    world.mission_objects[1].active = true;
    world.mission_objects[2].active = false;
    world.edit_mode.active = false;
    world.edit_mode.mode = 3;
    world.edit_mode.dialog_open = true;

    WseVmState state{};
    state.options = &options;
    state.world = &world;

    // Inactive edit mode queries.
    expect(run(opcode(WseOpcode::edit_mode_get_num_selected_prop_instances),
               {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == -1 &&
               run(opcode(WseOpcode::edit_mode_get_mode), {0}, {0}, effect, {},
                   {}, state) == WseDispatch::handled &&
               effect.value == 3 &&
               run(opcode(WseOpcode::edit_mode_in_edit_objects_mode), {}, {},
                   effect, {}, {}, state) == WseDispatch::condition_false &&
               run(opcode(WseOpcode::edit_mode_window_open), {}, {}, effect, {},
                   {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::ignored,
           "edit_mode queries handle inactive state and expose get_mode/dialog_open");

    // 4606 option flag + active prop selection & highlighted index adjustment.
    world.edit_mode.active = true;
    world.edit_mode.mode = 0;
    expect(!options.edit_mode_enabled &&
               run(opcode(WseOpcode::edit_mode_set_enabled), {1}, {1}, effect,
                   {}, {}, state) == WseDispatch::handled_no_store &&
               options.edit_mode_enabled &&
               run(opcode(WseOpcode::edit_mode_in_edit_objects_mode), {}, {},
                   effect, {}, {}, state) == WseDispatch::handled_no_store,
           "edit_mode_set_enabled and edit_mode_in_edit_objects_mode reflect state");

    expect(run(opcode(WseOpcode::edit_mode_select_prop_instance), {2}, {2},
               effect, {}, {}, state) == WseDispatch::ignored &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {1}, {1},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_get_num_selected_prop_instances),
                   {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 2 &&
               run(opcode(WseOpcode::edit_mode_get_selected_prop_instance),
                   {0, 1}, {0, 1}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
               effect.value == 1 &&
               run(opcode(WseOpcode::edit_mode_get_selected_prop_instance),
                   {0, 9}, {0, 9}, effect, {}, {}, state) ==
                   WseDispatch::handled &&
               effect.value == -1,
           "edit_mode prop selection deduplicates active instances and bounds index queries");

    // Highlight prop 1 (index 1), then deselect earlier prop 0 -> index decrements to 0.
    expect(run(opcode(WseOpcode::edit_mode_set_highlighted_prop_instance), {1},
               {1}, effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_get_highlighted_prop_instance),
                   {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 1 &&
               run(opcode(WseOpcode::edit_mode_deselect_prop_instance), {0},
                   {0}, effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.edit_mode.highlighted_index == 0 &&
               run(opcode(WseOpcode::edit_mode_get_highlighted_prop_instance),
                   {0}, {0}, effect, {}, {}, state) == WseDispatch::handled &&
               effect.value == 1 &&
               run(opcode(WseOpcode::edit_mode_deselect_prop_instance), {1},
                   {1}, effect, {}, {}, state) == WseDispatch::handled_no_store &&
               world.edit_mode.highlighted_index == -1 &&
               run(opcode(WseOpcode::edit_mode_select_prop_instance), {0}, {0},
                   effect, {}, {}, state) == WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_set_highlighted_prop_instance),
                   {0}, {0}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               run(opcode(WseOpcode::edit_mode_set_highlighted_prop_instance),
                   {}, {}, effect, {}, {}, state) ==
                   WseDispatch::handled_no_store &&
               world.edit_mode.highlighted_index == -1,
           "edit_mode deselect shifts highlighted_index and omitted set_highlighted clears it");
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
    test_script_state_and_position_group();
    test_dictionary_group();
    test_feval();
    test_array_group();
    test_slot_gt_group();
    test_scalar_config_skill_savegame_group();
    test_troop_group();
    test_item_group();
    test_edit_mode_group();
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
