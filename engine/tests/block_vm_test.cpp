// Host tests for the reconstructed statement-block VM (wse/vm/block_vm.h)
// and its script/trigger integration.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/game/script.h"
#include "wse/game/trigger.h"
#include "wse/io/text_stream.h"
#include "wse/vm/block_vm.h"
#include "wse/vm/operand.h"
#include "wse/vm/statement.h"
#include "wse/vm/wse_opcodes.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

constexpr std::uint64_t tagged(std::uint8_t tag, std::uint64_t payload) noexcept {
    return (static_cast<std::uint64_t>(tag & 0x7FU) << 56) | (payload & wse::vm::kPayloadMask);
}

constexpr std::int64_t reg(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(1, static_cast<std::uint64_t>(idx)));
}

constexpr std::int64_t glob(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(2, static_cast<std::uint64_t>(idx)));
}

constexpr std::int64_t loc(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(0x11, static_cast<std::uint64_t>(idx)));
}

constexpr std::int64_t sreg(std::int32_t idx) noexcept {
    // Retail tag 0 string register (value fits directly as non-negative payload).
    return static_cast<std::int64_t>(idx);
}

wse::vm::Statement make_stmt(std::uint32_t opcode, std::initializer_list<std::int64_t> args) {
    wse::vm::Statement stmt{};
    stmt.opcode = opcode;
    stmt.numOperands = static_cast<std::int32_t>(args.size());
    std::size_t idx = 0;
    for (std::int64_t arg : args) {
        if (idx < wse::vm::kMaxOperands) {
            stmt.operands[idx++] = arg;
        }
    }
    return stmt;
}

bool test_hook(void* user,
               std::uint32_t opcode,
               std::span<const std::int64_t> wide,
               std::span<const std::int32_t>,
               std::span<const std::uint8_t>,
               const wse::vm::Statement& stmt,
               wse::vm::VmContext& ctx,
               std::span<std::int64_t> locals,
               bool& handled) {
    if (opcode == 999U) {
        handled = true;
        auto* counter = static_cast<int*>(user);
        ++(*counter);
        wse::vm::store_lvalue(static_cast<std::uint64_t>(stmt.operands[0]),
                              wide[1] + 1000LL, ctx, locals);
        return true;
    }
    return true;
}

void test_analyze_block() {
    using namespace wse::vm;

    StatementBlock block;
    block.statements = {
        make_stmt(core_op::kTryBegin, {}),                 // 0 -> end 6
        make_stmt(core_op::kEq, {reg(0), 1}),              // 1 -> end 3
        make_stmt(core_op::kAssign, {reg(1), 10}),         // 2 -> end 3
        make_stmt(core_op::kElseTry, {}),                  // 3 -> end 6
        make_stmt(core_op::kEq, {reg(0), 2}),              // 4 -> end 6
        make_stmt(core_op::kAssign, {loc(40), 20}),        // 5 -> end 6
        make_stmt(core_op::kTryEnd, {}),                   // 6 -> end 8 (unresolved tail)
        make_stmt(core_op::kAssign, {reg(2), 99}),         // 7 -> end 8
    };

    analyze_statement_block(block);
    expect(block.analyzed, "analyze sets analyzed flag");
    expect(block.statements[0].endStatement == 6, "try_begin closes at try_end");
    expect(block.statements[1].endStatement == 3, "if-body statement closes at else_try");
    expect(block.statements[2].endStatement == 3, "second if-body statement closes at else_try");
    expect(block.statements[3].endStatement == 6, "else_try closes at try_end");
    expect(block.statements[4].endStatement == 6, "else-body statement closes at try_end");
    expect(block.statements[5].endStatement == 6, "last else-body statement closes at try_end");
    expect(block.statements[6].endStatement == 8, "try_end gets block size in pass 2");
    expect(block.statements[7].endStatement == 8, "top-level trailing statement gets block size");
    expect(block.numLocalVariablesUsed == 41, "local count scans up to max index + 1");

    StatementBlock emptyBlock;
    emptyBlock.statements = {make_stmt(core_op::kAssign, {loc(2), 1})};
    analyze_statement_block(emptyBlock);
    expect(emptyBlock.numLocalVariablesUsed == 32, "minimum local variable count is 32");

    StatementBlock hugeLocalBlock;
    hugeLocalBlock.statements = {make_stmt(core_op::kAssign, {loc(1500), 1})};
    analyze_statement_block(hugeLocalBlock);
    expect(hugeLocalBlock.numLocalVariablesUsed == 1024, "maximum local variable count is 1024");
}

void test_modifiers_and_top_level_failure() {
    using namespace wse::vm;

    VmContext ctx;
    StatementBlock passOrBlock;
    passOrBlock.statements = {
        make_stmt(core_op::kEq | kThisOrNextFlag, {1, 1}),
        make_stmt(core_op::kAssign, {reg(0), 77}),
        make_stmt(core_op::kAssign, {reg(1), 88}),
    };
    expect(execute_statement_block(passOrBlock, ctx), "this_or_next pass block succeeds");
    expect(ctx.registers[0] == 0, "this_or_next skips next statement when first succeeds");
    expect(ctx.registers[1] == 88, "statement after skipped one still executes");

    StatementBlock chainOrBlock;
    chainOrBlock.statements = {
        make_stmt(core_op::kEq | kThisOrNextFlag, {1, 2}),
        make_stmt(core_op::kGt | kThisOrNextFlag, {5, 3}),
        make_stmt(core_op::kAssign, {reg(2), 55}),
        make_stmt(core_op::kEq | kNegFlag, {4, 5}),
    };
    expect(execute_statement_block(chainOrBlock, ctx), "OR chain second branch + neg passes");
    expect(ctx.registers[2] == 0, "last statement in OR chain is skipped after branch succeeds");

    StatementBlock failBlock;
    failBlock.statements = {
        make_stmt(core_op::kEq | kThisOrNextFlag, {1, 2}),
        make_stmt(core_op::kEq, {3, 4}),
        make_stmt(core_op::kAssign, {reg(3), 123}),
    };
    expect(!execute_statement_block(failBlock, ctx), "top-level failure returns false");
    expect(ctx.registers[3] == 0, "top-level failure aborts block before trailing assign");

    StatementBlock staleSkipBlock;
    ctx.skipStatement = true;
    staleSkipBlock.statements = {make_stmt(core_op::kAssign, {reg(4), 44})};
    expect(execute_statement_block(staleSkipBlock, ctx) && ctx.registers[4] == 44,
           "g_skipStatement resets on block entry");
}

void test_try_else_and_loops() {
    using namespace wse::vm;

    // Chained try_begin / else_try / else_try / try_end.
    auto runBranch = [](std::int64_t selector) {
        VmContext ctx;
        ctx.registers[0] = selector;
        StatementBlock block;
        block.statements = {
            make_stmt(core_op::kTryBegin, {}),
            make_stmt(core_op::kEq, {reg(0), 1}),
            make_stmt(core_op::kAssign, {reg(1), 100}),
            make_stmt(core_op::kElseTry, {}),
            make_stmt(core_op::kEq, {reg(0), 2}),
            make_stmt(core_op::kAssign, {reg(1), 200}),
            make_stmt(core_op::kElseTry, {}),
            make_stmt(core_op::kAssign, {reg(1), 300}),
            make_stmt(core_op::kTryEnd, {}),
        };
        expect(execute_statement_block(block, ctx), "chained try/else block succeeds");
        return ctx.registers[1];
    };
    expect(runBranch(1) == 100, "chained try first branch executes");
    expect(runBranch(2) == 200, "chained try second branch executes");
    expect(runBranch(9) == 300, "chained try fallback else executes");

    // try_for_range with continue_loop and break_loop nested inside try_begin.
    VmContext ctx;
    StatementBlock loopBlock;
    loopBlock.statements = {
        make_stmt(core_op::kAssign, {loc(0), 0}),
        make_stmt(core_op::kTryForRange, {loc(1), 0, 10}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(core_op::kEq, {loc(1), 3}),
        make_stmt(core_op::kContinueLoop, {}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(core_op::kGe, {loc(1), 6}),
        make_stmt(core_op::kBreakLoop, {}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kValAdd, {loc(0), loc(1)}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kAssign, {glob(5), loc(0)}),
    };
    expect(execute_statement_block(loopBlock, ctx), "loop with nested continue/break succeeds");
    // 0 + 1 + 2 + (skip 3) + 4 + 5 + (break on 6) = 12.
    expect(ctx.globalVariables.get(5) == 12, "continue skips 3 and break exits at 6");

    // Empty loop: initial value stored but 0 iterations executed.
    StatementBlock emptyLoop;
    emptyLoop.statements = {
        make_stmt(core_op::kAssign, {reg(7), 999}),
        make_stmt(core_op::kTryForRange, {reg(6), 5, 5}),
        make_stmt(core_op::kAssign, {reg(7), 111}),
        make_stmt(core_op::kTryEnd, {}),
    };
    expect(execute_statement_block(emptyLoop, ctx), "empty try_for_range succeeds");
    expect(ctx.registers[6] == 5 && ctx.registers[7] == 999,
           "empty try_for_range sets loop counter once and skips body");

    // Backwards loop [2, 5) -> 4, 3, 2.
    StatementBlock backLoop;
    backLoop.statements = {
        make_stmt(core_op::kAssign, {reg(8), 0}),
        make_stmt(core_op::kTryForRangeBackwards, {loc(2), 2, 5}),
        make_stmt(core_op::kValMul, {reg(8), 10}),
        make_stmt(core_op::kValAdd, {reg(8), loc(2)}),
        make_stmt(core_op::kTryEnd, {}),
    };
    expect(execute_statement_block(backLoop, ctx), "backwards loop executes");
    expect(ctx.registers[8] == 432, "backwards loop visits 4, 3, 2 in order");

    // Entity iterator without world data runs 0 times and preserves block execution.
    StatementBlock agentLoop;
    agentLoop.statements = {
        make_stmt(core_op::kTryForAgents, {loc(3)}),
        make_stmt(core_op::kAssign, {reg(9), 1}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kAssign, {reg(10), 7}),
    };
    expect(execute_statement_block(agentLoop, ctx) && ctx.registers[9] == 0 && ctx.registers[10] == 7,
           "try_for_agents exits cleanly and continues block");
}

void test_native_core_math_and_hook() {
    using namespace wse::vm;

    VmContext ctx;
    int hookCalls = 0;
    ctx.nativeHook = test_hook;
    ctx.nativeHookUser = &hookCalls;

    StatementBlock mathBlock;
    mathBlock.statements = {
        make_stmt(core_op::kAssign, {reg(0), 25}),
        make_stmt(core_op::kValClamp, {reg(0), 0, 10}),       // exclusive upper -> 9!
        make_stmt(core_op::kStoreAdd, {reg(1), reg(0), 11}),  // 20
        make_stmt(core_op::kStoreSub, {reg(2), reg(1), 7}),   // 13
        make_stmt(core_op::kStoreMul, {reg(3), reg(2), 3}),   // 39
        make_stmt(core_op::kStoreDiv, {reg(4), reg(3), 4}),   // 9
        make_stmt(core_op::kStoreMod, {reg(5), reg(3), 5}),   // 4
        make_stmt(core_op::kAssign, {reg(6), -17}),
        make_stmt(core_op::kValAbs, {reg(6)}),                // 17
        make_stmt(core_op::kValLshift, {reg(4), 2}),          // 36
        make_stmt(core_op::kSetFixedPointMultiplier, {1000}),
        make_stmt(core_op::kSetFixedPointMultiplier, {0}),    // ignored (<= 0)
        make_stmt(core_op::kAssign, {reg(7), 9}),
        make_stmt(core_op::kConvertToFixedPoint, {reg(7)}),   // 9000
        make_stmt(core_op::kStoreSqrt, {reg(8), reg(7)}),     // sqrt(9)*1000 = 3000
        make_stmt(999U, {reg(9), 42}),                        // native hook
        make_stmt(core_op::kIsBetween, {reg(0), 0, 10}),      // 9 in [0, 10) -> true
    };
    expect(execute_statement_block(mathBlock, ctx), "math + fixed-point + hook block succeeds");
    expect(ctx.registers[0] == 9, "val_clamp clamps upper exclusive (upper - 1)");
    expect(ctx.registers[1] == 20 && ctx.registers[2] == 13 && ctx.registers[3] == 39,
           "store_add/sub/mul match");
    expect(ctx.registers[4] == 36 && ctx.registers[5] == 4, "store_div/mod + lshift match");
    expect(ctx.registers[6] == 17, "val_abs negates negative values");
    expect(ctx.fixedPointMultiplier == 1000 && ctx.registers[7] == 9000 && ctx.registers[8] == 3000,
           "fixed-point multiplier & store_sqrt match");
    expect(hookCalls == 1 && ctx.registers[9] == 1042, "nativeHook intercepts custom opcode");

    // Division and modulo by zero match retail diagnostics + sentinel.
    StatementBlock divZeroBlock;
    divZeroBlock.statements = {
        make_stmt(core_op::kAssign, {reg(10), 123}),
        make_stmt(core_op::kValDiv, {reg(10), 0}),
        make_stmt(core_op::kAssign, {reg(11), 55}),
        make_stmt(core_op::kValMod, {reg(11), 0}),
        make_stmt(core_op::kAssign, {12, 99}),  // illegal immediate lvalue
    };
    expect(execute_statement_block(divZeroBlock, ctx), "div/mod zero + illegal lvalue don't crash");
    expect(ctx.registers[10] == kDivisionByZeroSentinel, "div by zero writes 40000000");
    expect(ctx.registers[11] == 55, "mod by zero preserves destination register");
    expect(ctx.errorCount == 3, "div0 + mod0 + illegal lvalue record three diagnostics");
}

void test_scripts_recursion_and_depth_limit() {
    using namespace wse::vm;
    using namespace wse::game;

    // Recursive factorial in Warband scripts.txt format:
    // script 0 "fact": (n) -> reg0 += ... actually compute into reg0 cleanly:
    //   n <= 1: assign reg0, 1
    //   else: n1 = n - 1; call_script(0, n1); val_mul reg0, n
    const std::string scriptsTxt =
        "scriptsfile version 1\n"
        "3\n"
        // Script 0: fact(n) -> writes result to reg0
        "fact -1\n"
        "9\n"
        "23 2 " + std::to_string(loc(0)) + " 1\n"      // store_script_param :n, 1
        "4 0\n"                                         // try_begin
        "32 2 2 " + std::to_string(loc(0)) + "\n"       // gt 2, :n  (i.e. :n <= 1)
        "2133 2 " + std::to_string(reg(0)) + " 1\n"     // assign reg0, 1
        "5 0\n"                                         // else_try
        "2121 3 " + std::to_string(loc(1)) + " " + std::to_string(loc(0)) + " 1\n"  // :n-1
        "1 2 0 " + std::to_string(loc(1)) + "\n"        // call_script fact(:n-1)
        "2107 2 " + std::to_string(reg(0)) + " " + std::to_string(loc(0)) + "\n"    // reg0 *= :n
        "3 0\n"                                         // try_end
        // Script 1: sum3(a, b, c) testing store_script_param_1/2/n
        "sum3 -1\n"
        "5\n"
        "21 1 " + std::to_string(loc(0)) + "\n"
        "22 1 " + std::to_string(loc(1)) + "\n"
        "23 2 " + std::to_string(loc(2)) + " 3\n"
        "2120 3 " + std::to_string(reg(1)) + " " + std::to_string(loc(0)) + " " + std::to_string(loc(1)) + "\n"
        "2105 2 " + std::to_string(reg(1)) + " " + std::to_string(loc(2)) + "\n"
        // Script 2: infinite self recursion to test 255 depth limit inside try_begin
        "inf -1\n"
        "5\n"
        "4 0\n"
        "1 1 2\n"                                       // call_script 2
        "5 0\n"
        "2105 2 " + std::to_string(reg(2)) + " 1\n"     // increments reg2 when callee hits max depth!
        "3 0\n";

    wse::io::TokenReader reader(scriptsTxt);
    ScriptManager manager;
    expect(read_script_manager(reader, manager) == ReadError::none, "VM test scripts.txt reads");

    VmContext ctx;
    std::int64_t arg = 6;
    expect(execute_script(manager, "fact", ctx, std::span<const std::int64_t>(&arg, 1)),
           "recursive fact(6) script runs");
    expect(ctx.registers[0] == 720, "factorial preserves locals across recursion (6! == 720)");
    expect(ctx.errorCount == 0, "valid recursive scripts log no errors");

    const std::int64_t args3[3] = {10, 20, 12};
    expect(execute_script(manager, 1, ctx, args3), "sum3 script runs");
    expect(ctx.registers[1] == 42, "store_script_param_1/2/3 store 10 + 20 + 12 == 42");

    // Exhaust 255 recursion limit safely.
    expect(execute_script(manager, 2, ctx), "unbounded recursion catches maxCallDepth in try/else");
    expect(ctx.maxObservedDepth == kMaxCallDepth + 1, "recursion reaches exact max depth (256 frames)");
    expect(ctx.registers[2] == 1, "caller catching failed depth call enters else_try once");
}

void test_triggers_and_wse_integration() {
    using namespace wse::vm;
    using namespace wse::game;

    // Event trigger (-52.000000): checks store_current_trigger == -52 and sets string/register state.
    const std::string triggersTxt =
        "triggersfile version 1\n"
        "2\n"
        "-52.0 0.0 0.0\n"
        "2\n" +
        std::to_string(static_cast<std::uint32_t>(WseOpcode::store_current_trigger)) + " 1 " + std::to_string(loc(0)) + "\n" +
        "31 2 " + std::to_string(loc(0)) + " -52\n" +
        "2\n" +
        std::to_string(static_cast<std::uint32_t>(WseOpcode::str_store_upper)) + " 2 " + std::to_string(sreg(0)) + " " + std::to_string(sreg(1)) + "\n" +
        std::to_string(static_cast<std::uint32_t>(WseOpcode::str_length)) + " 2 " + std::to_string(reg(0)) + " " + std::to_string(sreg(0)) + "\n" +
        // Delayed interval trigger (1.0s check, 2.0s delay, 3.0s rearm)
        "1.0 2.0 3.0\n"
        "1\n"
        "31 2 " + std::to_string(glob(0)) + " 1\n" +
        "1\n"
        "2133 2 " + std::to_string(glob(1)) + " 999\n";

    std::int64_t timersTotal[1] = {0};
    TriggerManager trigMgr;
    {
        wse::io::TokenReader reader(triggersTxt);
        expect(reader.skip_tokens(3) &&
                   read_trigger_manager(reader, trigMgr, "Game", timersTotal) == ReadError::none,
               "VM trigger fixture loads");
    }

    VmContext ctx;
    ctx.stringRegisters[1] = u"Warband";
    expect(execute_trigger_manager(trigMgr, -52, timersTotal, ctx),
           "negative event trigger executes through VM");
    expect(ctx.stringRegisters[0] == u"WARBAND" && ctx.registers[0] == 7,
           "WSE string opcodes run inside trigger consequences");
    expect(trigMgr.triggers.size() == 2U && trigMgr.triggers[0].status == 2,
           "event trigger arms rearm timer");

    // Advance timed trigger through condition (t=1s), delay (t=3s), rearm (t=6s).
    ctx.globalVariables.set(0, 1);
    timersTotal[0] = 100000;
    expect(execute_trigger_manager(trigMgr, 1, timersTotal, ctx), "timed trigger check passes at 1s");
    expect(trigMgr.triggers.size() == 2U && trigMgr.triggers[1].status == 1 &&
               ctx.globalVariables.get(1) == 0,
           "timed trigger enters delay status");
    timersTotal[0] = 300000;
    expect(execute_trigger_manager(trigMgr, 1, timersTotal, ctx), "timed trigger fires delay at 3s");
    expect(trigMgr.triggers.size() == 2U && trigMgr.triggers[1].status == 2 &&
               ctx.globalVariables.get(1) == 999,
           "timed trigger consequences mutate global variable slot");

    // Simple trigger invoking WSE register_set / register_get.
    const std::string simpleTxt =
        "simple_triggersfile version 1\n"
        "1\n"
        "0.5\n"
        "2\n" +
        std::to_string(static_cast<std::uint32_t>(WseOpcode::register_set)) + " 2 5 314\n" +
        std::to_string(static_cast<std::uint32_t>(WseOpcode::register_get)) + " 2 " + std::to_string(glob(2)) + " 5\n";

    SimpleTriggerManager simpleMgr;
    {
        wse::io::TokenReader reader(simpleTxt);
        expect(reader.skip_tokens(3) &&
                   read_simple_trigger_manager(reader, simpleMgr, "Simple", timersTotal) ==
                       ReadError::none,
               "VM simple trigger fixture loads");
    }
    timersTotal[0] = 400000;  // +1s since 300000 -> passes 0.5s check
    expect(execute_simple_trigger_manager(simpleMgr, 1, timersTotal, ctx),
           "simple trigger runs WSE ops");
    expect(ctx.registers[5] == 314 && ctx.globalVariables.get(2) == 314,
           "simple trigger mutates register and reads it into global slot");
}

}  // namespace

int main() {
    test_analyze_block();
    test_modifiers_and_top_level_failure();
    test_try_else_and_loops();
    test_native_core_math_and_hook();
    test_scripts_recursion_and_depth_limit();
    test_triggers_and_wse_integration();

    if (failures != 0) {
        std::printf("block_vm tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("block_vm tests: PASS\n");
    return 0;
}
