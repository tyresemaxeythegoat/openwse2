// Host tests for the portable bitwise and floating-point executeWSE handlers.
//
// Build/run: powershell -File engine/build_host.ps1

#include "wse_test_common.h"

namespace {

using namespace wse::test;

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

    dispatch = run(9999U, {1}, {}, effect);
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
    // Retail mbBasicGame::parseString (0x140072498) turns literal '^' into
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

}  // namespace

namespace wse::selftest {

int run_wse_bitwise_fp_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_enum_values();
    test_shift_group();
    test_bitwise_group();
    test_operand_counts();
    test_unimplemented_and_mask();
    test_fp_load_store();
    test_fp_compare();
    test_fp_arithmetic();
    test_fp_math();
    test_fp_errors();
    test_feval();
    if (failures != 0) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::printf("wse_bitwise_fp tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_wse_bitwise_fp_suite(nullptr);
}
#endif
