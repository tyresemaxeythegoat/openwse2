// Host tests for the reconstructed statement-block VM (wse/vm/block_vm.h)
// and its script/trigger integration.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
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

    // Phase 2C: mbSimpleTriggerManager::execute early-out when positive odd eventKey sees
    // queued window type 0x12 at the back of world.m5.enqueued_windows.
    {
        SimpleTriggerManager twoMgr;
        SimpleTrigger t0;
        t0.interval = 0.0;
        t0.timer.value = 0;
        t0.block.statements = {make_stmt(core_op::kAssign, {glob(10), 11})};
        SimpleTrigger t1;
        t1.interval = 0.0;
        t1.timer.value = 0;
        t1.block.statements = {make_stmt(core_op::kAssign, {glob(11), 22})};
        twoMgr.triggers = {t0, t1};

        VmContext oddCtx;
        oddCtx.world.m5.enqueued_windows.emplace_back(kQueuedWindowMenuType, -1);
        expect(execute_simple_trigger_manager(twoMgr, 1, timersTotal, oddCtx) &&
                   oddCtx.globalVariables.get(10) == 11 && oddCtx.globalVariables.get(11) == 0,
               "simple trigger manager stops after trigger 0 on odd eventKey with type-0x12 window");

        // Even eventKey or non-0x12 window runs both triggers normally.
        VmContext evenCtx;
        evenCtx.world.m5.enqueued_windows.emplace_back(kQueuedWindowMenuType, -1);
        expect(execute_simple_trigger_manager(twoMgr, 2, timersTotal, evenCtx) &&
                   evenCtx.globalVariables.get(10) == 11 && evenCtx.globalVariables.get(11) == 22,
               "simple trigger manager does not break on even eventKey");

        VmContext otherWinCtx;
        otherWinCtx.world.m5.enqueued_windows.emplace_back(0x11, -1);
        expect(execute_simple_trigger_manager(twoMgr, 1, timersTotal, otherWinCtx) &&
                   otherWinCtx.globalVariables.get(10) == 11 &&
                   otherWinCtx.globalVariables.get(11) == 22,
               "simple trigger manager ignores non-0x12 queued windows");
    }

    // End-to-end producer (`jump_to_menu` 2060) -> odd-key early-out -> consumer
    // (`consume_campaign_queued_window` matching `mbMapWindow::frameMove` + `mbMenuWindow::open`).
    {
        SimpleTriggerManager menuMgr;
        SimpleTrigger t0;
        t0.interval = 0.0;
        t0.timer.value = 0;
        t0.block.statements = {
            make_stmt(2060, {3}),
            make_stmt(core_op::kAssign, {glob(10), 11}),
        };
        SimpleTrigger t1;
        t1.interval = 0.0;
        t1.timer.value = 0;
        t1.block.statements = {make_stmt(core_op::kAssign, {glob(11), 22})};
        menuMgr.triggers = {t0, t1};

        VmContext menuCtx;
        expect(execute_simple_trigger_manager(menuMgr, 1, timersTotal, menuCtx) &&
                   menuCtx.world.jump_menu_no == 3 &&
                   menuCtx.world.m5.enqueued_windows.size() == 1U &&
                   menuCtx.world.m5.enqueued_windows.back().first == kQueuedWindowMenuType &&
                   menuCtx.globalVariables.get(10) == 11 &&
                   menuCtx.globalVariables.get(11) == 0,
               "jump_to_menu 2060 produces queued type-0x12 window that halts odd pass");

        const std::int32_t consumed = menuCtx.world.consume_campaign_queued_window();
        expect(consumed == kScreenWindowMenu &&
                   menuCtx.world.m5.active_window == kScreenWindowMenu &&
                   menuCtx.world.m6.ui.active_window_id == kScreenWindowMenu &&
                   menuCtx.world.m5.enqueued_windows.empty(),
               "campaign consumer pops type-0x12 menu window and syncs m6.ui.active_window_id");

        // `mbMapWindow::frameMove` (0x140118ae7) opens 0x12 (-> 0x12), 0x0f (-> 0x16), and
        // 0x10 (-> 0x15) from the back; every other type is popped into an unknown window (-1)
        // because the engine models no window that would consume it.
        menuCtx.world.enqueue_window(2);  // loot window (menu-only)
        menuCtx.world.enqueue_window(kQueuedWindowNotesType);
        expect(menuCtx.world.consume_campaign_queued_window() == kScreenWindowNotes &&
                   menuCtx.world.m5.active_window == kScreenWindowNotes &&
                   menuCtx.world.m6.ui.active_window_id == kScreenWindowNotes &&
                   menuCtx.world.m5.enqueued_windows.size() == 1U,
               "campaign consumer pops type-0x10 notes window into 0x15");
        menuCtx.world.m5.enqueued_windows.emplace_back(kQueuedWindowPresentationType, 7);
        expect(menuCtx.world.consume_campaign_queued_window() == kScreenWindowPresentation &&
                   menuCtx.world.active_presentation_no == 7 &&
                   menuCtx.world.m5.active_window == kScreenWindowPresentation &&
                   menuCtx.world.m6.ui.active_window_id == kScreenWindowPresentation &&
                   menuCtx.world.m5.enqueued_windows.size() == 1U,
               "campaign consumer pops type-0x0f presentation window into 0x16");
        expect(menuCtx.world.consume_campaign_queued_window() == -1 &&
                   menuCtx.world.m5.enqueued_windows.empty() &&
                   menuCtx.world.m5.active_window == -1 &&
                   menuCtx.world.m6.ui.active_window_id == -1,
               "campaign consumer drains a non-map window type into an unknown window");

        // Regression: a consumed menu must not gate map-only opcodes for the rest of the
        // session; `change_screen_map` (2052, type 0x11) from the menu clears it.
        menuCtx.world.enqueue_window(kQueuedWindowMenuType);
        (void)menuCtx.world.consume_campaign_queued_window();
        expect(menuCtx.world.m6.ui.active_window_id == kScreenWindowMenu,
               "menu window is active after jump_to_menu");
        menuCtx.world.enqueue_window(0x11);
        (void)menuCtx.world.consume_campaign_queued_window();
        expect(menuCtx.world.m6.ui.active_window_id == -1,
               "change_screen_map leaves the menu window so map-gated opcodes run again");

        // Regression: window types the map does not open no longer accumulate, and nothing
        // grows the queue past `kMaxQueuedWindows` even without a consumer.
        for (int i = 0; i < 1000; ++i) {
            menuCtx.world.enqueue_window(0x11);
            (void)menuCtx.world.consume_campaign_queued_window();
        }
        expect(menuCtx.world.m5.enqueued_windows.empty(),
               "campaign frames drain change_screen_map entries");
        for (int i = 0; i < 1000; ++i) {
            menuCtx.world.enqueue_window(kQueuedWindowPresentationType, i);
        }
        expect(menuCtx.world.m5.enqueued_windows.size() == kMaxQueuedWindows &&
                   menuCtx.world.m5.enqueued_windows.back().second == 999 &&
                   menuCtx.world.m5.enqueued_windows.front().second ==
                       1000 - static_cast<int>(kMaxQueuedWindows),
               "enqueue_window keeps the newest kMaxQueuedWindows entries");
        menuCtx.world.m5.enqueued_windows.clear();

        menuMgr.triggers[0].interval = 100.0;  // let next frame proceed to trigger 1 cleanly
        expect(execute_simple_trigger_manager(menuMgr, 1, timersTotal, menuCtx) &&
                   menuCtx.globalVariables.get(11) == 22,
               "later odd-key pass runs remaining simple triggers after window consumption");
    }

    // Phase 2E party container lifetimes: monotonic IDs + full `mbGame::removeParty` teardown.
    {
        VmContext pctx;
        pctx.world.has_game = true;
        pctx.world.parties.resize(2);
        pctx.world.parties[0].active = true;
        pctx.world.parties[1].active = true;
        pctx.world.party_templates.resize(1);
        pctx.world.party_templates[0].name = "Raiders";

        StatementBlock pSpawnBlock;
        pSpawnBlock.statements = {make_stmt(1100U, {0, 0}), make_stmt(1100U, {0, 0})};
        expect(execute_statement_block(pSpawnBlock, pctx),
               "1100 spawns two parties");
        expect(pctx.world.parties.size() == 4U &&
                   pctx.world.party_templates[0].num_created == 2,
               "party creation appends monotonic ids and counts template num_created");

        (void)pctx.world.attach_party(2, 0);
        (void)pctx.world.attach_party(3, 2);
        pctx.world.parties[1].behavior_object = 2;
        pctx.world.parties[1].default_behavior_object = 2;
        pctx.world.m6.player(1).party_no = 2;
        pctx.world.parties[2].player_no = 1;
        pctx.world.encountered_party_no = 2;
        pctx.world.encountered_party_2_no = 2;

        StatementBlock pRemoveBlock;
        pRemoveBlock.statements = {make_stmt(1232U, {2}), make_stmt(1100U, {0, 0})};
        expect(execute_statement_block(pRemoveBlock, pctx),
               "1232 remove_party and subsequent spawn execute");
        expect(!pctx.world.is_valid_party(2) &&
                   (pctx.world.parties[2].flags & 0x100ULL) != 0ULL &&
                   pctx.world.parties[0].attached_party_nos.empty() &&
                   pctx.world.parties[3].attached_to_party_no == -1 &&
                   pctx.world.parties[1].behavior_object == -1 &&
                   pctx.world.parties[1].default_behavior_object == -1 &&
                   pctx.world.m6.player(1).party_no == -1 &&
                   pctx.world.encountered_party_no == -1 &&
                   pctx.world.encountered_party_2_no == -1 &&
                   pctx.world.party_templates[0].num_destroyed == 1 &&
                   pctx.registers[0] == 4 && pctx.world.parties.size() == 5U,
               "remove_party performs full retail teardown and never reuses removed party handles");

        // Review regressions (e6c4d0e): the main party is never removed, and
        // party_attach_to_party (1660) cannot close an attachment cycle.
        pctx.world.main_party_no = 0;
        expect(!pctx.world.remove_party(0) && pctx.world.is_valid_party(0) &&
                   pctx.world.main_party_no == 0,
               "remove_party refuses the main party and keeps main_party_no");
        StatementBlock mainRemove;
        mainRemove.statements = {make_stmt(1232U, {0})};
        expect(execute_statement_block(mainRemove, pctx) && pctx.world.is_valid_party(0) &&
                   pctx.world.main_party_no == 0,
               "1232 remove_party on the main party is a no-op");

        expect(pctx.world.attach_party(3, 1) && pctx.world.attach_party(1, 4) &&
                   !pctx.world.attach_party(4, 3) && !pctx.world.attach_party(4, 4) &&
                   pctx.world.parties[4].attached_to_party_no == -1,
               "attach_party refuses self-attachment and cycles (4 -> 3 -> 1 -> 4)");
        StatementBlock cycleAttach;
        cycleAttach.statements = {make_stmt(1660U, {4, 3})};
        expect(execute_statement_block(cycleAttach, pctx) &&
                   pctx.world.parties[4].attached_to_party_no == -1 &&
                   pctx.world.parties[3].attached_party_nos.empty(),
               "1660 party_attach_to_party leaves cyclic requests unattached");
    }
}

void test_return_values_positions_and_dict_loop() {
    using namespace wse::vm;
    using namespace wse::game;

    // 1. return_values -> store_num_return_values / store_return_value across call_script.
    ScriptManager mgr;
    Script callee;
    callee.id = "ret_callee";
    callee.block.statements = {
        make_stmt(core_op::kAssign, {loc(1), 77}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values),
                  {42, glob(0), loc(1)}),
    };
    Script caller;
    caller.id = "ret_caller";
    caller.block.statements = {
        make_stmt(core_op::kCallScript, {0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::store_num_return_values),
                  {reg(0)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::store_return_value),
                  {reg(1), 2}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::store_return_value),
                  {reg(2), 3}),
    };
    mgr.scripts.push_back(std::move(callee));
    mgr.scripts.push_back(std::move(caller));

    VmContext ctx;
    ctx.globalVariables.set(0, 9001);
    expect(execute_script(mgr, 1, ctx) && ctx.numReturnValues == 3 &&
               ctx.registers[0] == 3 && ctx.registers[1] == 9001 &&
               ctx.registers[2] == 77,
           "return_values propagates across call_script in block VM");

    // 2. position_get_vector_to_position + fld_pos_{x,y,z} + fst.
    ctx.positions[1].o = {10.0f, 20.0f, 30.0f, 1.0f};
    ctx.positions[2].o = {13.0f, 24.0f, 30.0f, 1.0f};
    StatementBlock posBlock;
    posBlock.statements = {
        make_stmt(static_cast<std::uint32_t>(
                      WseOpcode::position_get_vector_to_position),
                  {reg(4), 0, 1, 2}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fld_pos_x), {0, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fld_pos_y), {1, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fld_pos_z), {2, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fst), {reg(5), 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fst), {reg(6), 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fst), {reg(7), 2}),
    };
    expect(execute_statement_block(posBlock, ctx) && ctx.registers[4] == 500 &&
               ctx.registers[5] == 1000 && ctx.registers[6] == 2000 &&
               ctx.registers[7] == 3000,
           "position_get_vector_to_position and fld_pos_{x,y,z} work in VM");

    // 3. try_for_dict_keys: sorted iteration, nested loops + continue/break,
    // empty/invalid dict skip, and invalid sreg error reporting.
    ctx.stringRegisters[10] = u"z_last";
    ctx.stringRegisters[11] = u"a_first";
    ctx.stringRegisters[12] = u"m_mid";
    ctx.stringRegisters[13] = u"val";
    ctx.stringRegisters[20] = u"k1";
    ctx.stringRegisters[21] = u"k2";
    ctx.stringRegisters[22] = u"k3";
    ctx.stringRegisters[23] = u"x";
    ctx.stringRegisters[24] = u"y";
    ctx.stringRegisters[4] = u"sentinel_empty";
    ctx.stringRegisters[5] = u"sentinel_invalid";

    StatementBlock dictLoopBlock;
    dictLoopBlock.statements = {
        // dict0: {"a_first", "m_mid", "z_last"} -> loc(0)
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_create), {loc(0)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(0), sreg(10), sreg(13)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(0), sreg(11), sreg(13)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(0), sreg(12), sreg(13)}),
        // Check exact sorted visit order using base-10 positional accumulator reg(10).
        make_stmt(core_op::kAssign, {loc(3), 0}),
        make_stmt(core_op::kAssign, {reg(10), 0}),
        make_stmt(core_op::kTryForDictKeys, {sreg(0), loc(0)}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(core_op::kEq, {loc(3), 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_equals),
                  {sreg(0), sreg(11)}),
        make_stmt(core_op::kValAdd, {reg(10), 1}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(core_op::kEq, {loc(3), 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_equals),
                  {sreg(0), sreg(12)}),
        make_stmt(core_op::kValAdd, {reg(10), 10}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(core_op::kEq, {loc(3), 2}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_equals),
                  {sreg(0), sreg(10)}),
        make_stmt(core_op::kValAdd, {reg(10), 100}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kValAdd, {loc(3), 1}),
        make_stmt(core_op::kTryEnd, {}),

        // Nested try_for_dict_keys with inner continue_loop and outer break_loop.
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_create), {loc(1)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(1), sreg(20), sreg(13)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(1), sreg(21), sreg(13)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(1), sreg(22), sreg(13)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_create), {loc(2)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(2), sreg(23), sreg(13)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_set_str),
                  {loc(2), sreg(24), sreg(13)}),

        make_stmt(core_op::kAssign, {reg(11), 0}),
        make_stmt(core_op::kAssign, {reg(12), 0}),
        make_stmt(core_op::kTryForDictKeys, {sreg(1), loc(1)}),
        make_stmt(core_op::kValAdd, {reg(12), 1}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_equals),
                  {sreg(1), sreg(22)}),
        make_stmt(core_op::kBreakLoop, {}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kTryForDictKeys, {sreg(2), loc(2)}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_equals),
                  {sreg(2), sreg(23)}),
        make_stmt(core_op::kContinueLoop, {}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kValAdd, {reg(11), 1}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kTryEnd, {}),

        // Empty dict loop skips body without clobbering sreg(4).
        make_stmt(static_cast<std::uint32_t>(WseOpcode::dict_create), {loc(4)}),
        make_stmt(core_op::kAssign, {reg(13), 55}),
        make_stmt(core_op::kTryForDictKeys, {sreg(4), loc(4)}),
        make_stmt(core_op::kAssign, {reg(13), 99}),
        make_stmt(core_op::kTryEnd, {}),

        // Invalid dict ID skips body without clobbering sreg(5).
        make_stmt(core_op::kAssign, {reg(14), 1}),
        make_stmt(core_op::kTryForDictKeys, {sreg(5), -1}),
        make_stmt(core_op::kAssign, {reg(14), 2}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kAssign, {reg(15), 88}),
    };
    expect(execute_statement_block(dictLoopBlock, ctx) &&
               ctx.registers[10] == 111 && ctx.registers[11] == 2 &&
               ctx.registers[12] == 3 && ctx.registers[13] == 55 &&
               ctx.registers[14] == 1 && ctx.registers[15] == 88 &&
               ctx.stringRegisters[4] == u"sentinel_empty" &&
               ctx.stringRegisters[5] == u"sentinel_invalid" &&
               ctx.errorCount == 0,
           "try_for_dict_keys visits sorted keys, supports nesting/break/continue, and skips empty/invalid dicts");

    // Invalid / out-of-range string register reports error cleanly.
    StatementBlock badSregLoop;
    badSregLoop.statements = {
        make_stmt(core_op::kTryForDictKeys, {sreg(128), 0}),
        make_stmt(core_op::kTryEnd, {}),
    };
    expect(execute_statement_block(badSregLoop, ctx) && ctx.errorCount == 3 &&
               ctx.lastError.find("string register") != std::string::npos,
           "try_for_dict_keys reports out-of-range string register errors");
}

void test_feval_and_custom_array_sort() {
    using namespace wse::vm;

    VmContext ctx;
    wse::game::ScriptManager scripts;
    scripts.scripts.resize(4);

    // Script 0: cmp_int_desc using reg0 >= reg1 + verifying script params & depth isolation.
    scripts.scripts[0].id = "cmp_int_desc";
    scripts.scripts[0].block.statements = {
        make_stmt(core_op::kStoreScriptParam1, {loc(0)}),
        make_stmt(core_op::kStoreScriptParam2, {loc(1)}),
        make_stmt(core_op::kGe, {loc(0), 0}),
        make_stmt(core_op::kGe, {loc(1), 0}),
        make_stmt(core_op::kValAdd, {reg(20), 1}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(core_op::kGe, {reg(0), reg(1)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {1}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {0}),
        make_stmt(core_op::kTryEnd, {}),
    };

    // Script 1: cmp_str_len_then_lex on s0 vs s1.
    scripts.scripts[1].id = "cmp_str_len_then_lex";
    scripts.scripts[1].block.statements = {
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_length), {reg(3), sreg(0)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_length), {reg(4), sreg(1)}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(core_op::kGt, {reg(4), reg(3)}),  // len(s0) < len(s1)
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {1}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(core_op::kEq, {reg(3), reg(4)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::str_compare), {reg(5), sreg(0), sreg(1)}),
        make_stmt(core_op::kGt, {1, reg(5)}),  // reg(5) <= 0
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {1}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {0}),
        make_stmt(core_op::kTryEnd, {}),
    };

    // Script 2: cmp_pos_x via feval on pos0 vs pos1.
    scripts.scripts[2].id = "cmp_pos_x";
    scripts.scripts[2].block.statements = {
        make_stmt(static_cast<std::uint32_t>(WseOpcode::feval), {sreg(30)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fst), {reg(3), 0}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(core_op::kGe, {reg(3), 100}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {1}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {0}),
        make_stmt(core_op::kTryEnd, {}),
    };

    // Script 3: broken comparator always returning 0 (tests max double-pass abort).
    scripts.scripts[3].id = "cmp_broken";
    scripts.scripts[3].block.statements = {
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {0}),
    };

    ctx.scripts = &scripts;
    ctx.stringRegisters[30] = u"fp0 = (pos0x <= pos1x)";
    ctx.stringRegisters[31] = u"fp1 = {reg10} + 2.5";
    ctx.stringRegisters[10] = u"pear";
    ctx.stringRegisters[11] = u"a";
    ctx.stringRegisters[12] = u"kiwi";
    ctx.stringRegisters[13] = u"hi";

    ctx.positions[10] = WseTransform::identity();
    ctx.positions[10].o.x = 30.0f;
    ctx.positions[11] = WseTransform::identity();
    ctx.positions[11].o.x = -5.0f;
    ctx.positions[12] = WseTransform::identity();
    ctx.positions[12].o.x = 12.0f;

    StatementBlock driver;
    driver.statements = {
        // Seed caller local 0 and verify nested array_sort_custom does not clobber it.
        make_stmt(core_op::kAssign, {loc(0), 7777}),
        // Test feval inside VM with {reg10} substitution.
        make_stmt(core_op::kAssign, {reg(10), 4}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::feval), {sreg(31)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::fst), {reg(11), 1}),
        // Seed stale return_values before array_sort_custom.
        make_stmt(static_cast<std::uint32_t>(WseOpcode::return_values), {99, 88}),

        // 1) 2D int array (4x2): (10, 0), (30, 1), (30, 2), (20, 3) -> desc by col 0.
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_create), {loc(1), 0, 4, 2}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 10, 0, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 0, 0, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 30, 1, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 1, 1, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 30, 2, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 2, 2, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 20, 3, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(1), 3, 3, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_sort_custom), {loc(1), 0, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {reg(21), loc(1), 0, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {reg(22), loc(1), 1, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {reg(23), loc(1), 2, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {reg(24), loc(1), 3, 1}),

        // 2) 1D string array: ["pear", "a", "kiwi", "hi"] -> ["a", "hi", "kiwi", "pear"].
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_create), {loc(2), 1, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_push), {loc(2), sreg(10)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_push), {loc(2), sreg(11)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_push), {loc(2), sreg(12)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_push), {loc(2), sreg(13)}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_sort_custom), {loc(2), 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {sreg(20), loc(2), 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {sreg(21), loc(2), 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {sreg(22), loc(2), 2}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {sreg(23), loc(2), 3}),

        // 3) 1D pos array: [x=30, x=-5, x=12] -> [-5, 12, 30].
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_create), {loc(3), 2, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_push), {loc(3), 10}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_push), {loc(3), 11}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_push), {loc(3), 12}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_sort_custom), {loc(3), 2}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {25, loc(3), 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {26, loc(3), 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {27, loc(3), 2}),

        // 4) Broken comparator aborts without mutating [5, 1, 9].
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_create), {loc(4), 0, 3}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(4), 5, 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(4), 1, 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_set_val), {loc(4), 9, 2}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_sort_custom), {loc(4), 3}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {reg(25), loc(4), 0}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {reg(26), loc(4), 1}),
        make_stmt(static_cast<std::uint32_t>(WseOpcode::array_get_val), {reg(27), loc(4), 2}),

        // Verify caller local 0 survived all comparator callbacks intact!
        make_stmt(core_op::kAssign, {reg(29), loc(0)}),
    };

    expect(execute_statement_block(driver, ctx) && ctx.errorCount == 0 &&
               ctx.registers[11] == 650 && ctx.registers[20] > 0 &&
               ctx.registers[21] == 1 && ctx.registers[22] == 2 &&
               ctx.registers[23] == 3 && ctx.registers[24] == 0 &&
               ctx.stringRegisters[20] == u"a" &&
               ctx.stringRegisters[21] == u"hi" &&
               ctx.stringRegisters[22] == u"kiwi" &&
               ctx.stringRegisters[23] == u"pear" &&
               ctx.positions[25].o.x == -5.0f &&
               ctx.positions[26].o.x == 12.0f &&
               ctx.positions[27].o.x == 30.0f &&
               ctx.registers[25] == 5 && ctx.registers[26] == 1 &&
               ctx.registers[27] == 9 &&
               ctx.registers[29] == 7777 && ctx.currentDepth == 0 &&
               ctx.maxObservedDepth >= 1,
           "feval and array_sort_custom work across int/str/pos comparators, depth frames, and abort guard");
}

void test_world_state_script_integration() {
    using namespace wse::vm;

    auto op = [](WseOpcode value) { return static_cast<std::uint32_t>(value); };

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.world.has_game = true;
    ctx.world.water_level = 2.5f;
    ctx.world.skills[wse::game::kSkillIronflesh].max_level = 10;
    ctx.world.troops.resize(1);
    ctx.world.troops[0].attributes[0] = 10;
    ctx.world.troops[0].hit_points = 45.0f;
    ctx.world.troops[0].slots.set(7, 30);
    ctx.world.items.resize(1);
    ctx.world.items[0].id = "itm_broad_sword";
    ctx.world.mission_objects.resize(1);
    ctx.world.mission_objects[0].active = true;
    ctx.world.edit_mode.active = true;

    StatementBlock block;
    block.statements = {
        make_stmt(op(WseOpcode::options_set_cheat_mode), {1}),
        make_stmt(op(WseOpcode::stop_time), {1}),
        make_stmt(op(WseOpcode::troop_set_skill),
                  {0, wse::game::kSkillIronflesh, 4}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(op(WseOpcode::troop_slot_gt), {0, 7, 40}),
        make_stmt(core_op::kAssign, {reg(1), 999}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(core_op::kAssign, {reg(1), 10}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(op(WseOpcode::troop_set_attribute), {0, 0, 20}),
        make_stmt(op(WseOpcode::str_store_item_id), {sreg(0), 0}),
        make_stmt(op(WseOpcode::edit_mode_select_prop_instance), {0}),
        make_stmt(op(WseOpcode::edit_mode_get_num_selected_prop_instances),
                  {reg(2)}),
        make_stmt(op(WseOpcode::get_water_level), {reg(3)}),
    };

    const bool ok = execute_statement_block(block, ctx);
    expect(ok && ctx.errorCount == 0 && ctx.unimplementedCount == 0 &&
               ctx.world.paused &&
               ctx.world.troops[0].get_skill_raw(wse::game::kSkillIronflesh) ==
                   4 &&
               ctx.world.troops[0].attributes[0] == 20 &&
               ctx.registers[1] == 10 &&
               ctx.stringRegisters[0] == u"itm_broad_sword" &&
               ctx.registers[2] == 1 && ctx.registers[3] == 250,
           "statement block integrates WseWorldState troop, item, edit_mode, and config handlers");

    // Now set slot 7 above 40 and confirm troop_slot_gt takes the positive branch.
    ctx.world.troops[0].slots.set(7, 55);
    StatementBlock branchBlock;
    branchBlock.statements = {
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(op(WseOpcode::troop_slot_gt), {0, 7, 40}),
        make_stmt(core_op::kAssign, {reg(4), 1}),
        make_stmt(core_op::kElseTry, {}),
        make_stmt(core_op::kAssign, {reg(4), 0}),
        make_stmt(core_op::kTryEnd, {}),
    };
    expect(execute_statement_block(branchBlock, ctx) && ctx.registers[4] == 1 &&
               ctx.errorCount == 0 && ctx.unimplementedCount == 0 &&
               ctx.unimplementedOpcodeCounts.empty(),
           "troop_slot_gt branches true once slot exceeds threshold with loud check enabled");
}

void test_unimplemented_histogram_and_loud_failure() {
    using namespace wse::vm;

    // 1. dispatch_native_statement returns explicit NativeDispatch status values.
    {
        VmContext ctx;
        std::array<std::int64_t, 128> locals{};
        std::array<std::int64_t, 4> wide{5, 3, 0, 0};
        std::array<std::int32_t, 4> narrow{5, 3, 0, 0};
        std::array<std::uint8_t, 4> tags{};
        Statement stmt{};

        expect(dispatch_native_statement(core_op::kGe, wide, narrow, tags, stmt, ctx, locals) ==
                       NativeDispatch::handled &&
                   describe(NativeDispatch::handled) == "handled",
               "native handled condition reports NativeDispatch::handled");

        wide = {2, 9, 0, 0};
        expect(dispatch_native_statement(core_op::kGe, wide, narrow, tags, stmt, ctx, locals) ==
                       NativeDispatch::condition_false &&
                   describe(NativeDispatch::condition_false) == "false condition",
               "native false condition reports NativeDispatch::condition_false");

        expect(dispatch_native_statement(80U, wide, narrow, tags, stmt, ctx, locals) ==
                       NativeDispatch::unimplemented &&
                   describe(NativeDispatch::unimplemented) == "unimplemented native opcode" &&
                   ctx.unimplementedCount == 0,
               "dispatch_native_statement returns NativeDispatch::unimplemented cleanly");
    }

    // 2. Default compatibility mode records per-opcode histogram without aborting.
    {
        VmContext ctx;
        StatementBlock compatBlock;
        compatBlock.statements = {
            make_stmt(80U, {reg(3)}),   // unused native slot 80
            make_stmt(81U, {5}),        // unused native slot 81
            make_stmt(80U, {reg(4)}),
            make_stmt(9999U, {reg(5)}), // unimplemented out-of-range WSE slot
            make_stmt(13U, {}),         // unused control-flow slot (<30)
            make_stmt(core_op::kAssign, {reg(1), 42}),
        };
        expect(execute_statement_block(compatBlock, ctx) && ctx.errorCount == 0 &&
                   ctx.unimplementedCount == 5 && ctx.lastUnimplementedOpcode == 13U &&
                   ctx.registers[1] == 42,
               "compatibility mode tracks unimplemented hits while falling through true");
        expect(ctx.unimplementedOpcodeCounts.size() == 4U &&
                   ctx.unimplementedOpcodeCounts[80U] == 2U &&
                   ctx.unimplementedOpcodeCounts[81U] == 1U &&
                   ctx.unimplementedOpcodeCounts[9999U] == 1U &&
                   ctx.unimplementedOpcodeCounts[13U] == 1U,
               "per-opcode histogram records exact native, WSE, and control-flow counts");
    }

    // 3. Loud mode (failOnUnimplemented = true) aborts before else_try, neg, this_or_next, or call_script recovery.
    {
        VmContext ctx;
        ctx.failOnUnimplemented = true;
        StatementBlock loudNativeBlock;
        loudNativeBlock.statements = {
            make_stmt(core_op::kTryBegin, {}),
            make_stmt(82U, {0}),  // unused native slot 82
            make_stmt(core_op::kAssign, {reg(0), 111}),
            make_stmt(core_op::kElseTry, {}),
            make_stmt(core_op::kAssign, {reg(0), 222}),  // must NEVER execute!
            make_stmt(core_op::kTryEnd, {}),
        };
        expect(!execute_statement_block(loudNativeBlock, ctx) && ctx.registers[0] == 0 &&
                   ctx.errorCount == 1 && ctx.unimplementedCount == 1 &&
                   ctx.unimplementedOpcodeCounts[82U] == 1U &&
                   ctx.lastError.find("82") != std::string::npos,
               "failOnUnimplemented aborts before else_try can recover");

        StatementBlock negBlock;
        negBlock.statements = {
            make_stmt(kNegFlag | 82U, {0}),
            make_stmt(core_op::kAssign, {reg(1), 99}),
        };
        expect(!execute_statement_block(negBlock, ctx) && ctx.registers[1] == 0 &&
                   ctx.unimplementedOpcodeCounts[82U] == 2U && ctx.errorCount == 2,
               "neg flag cannot invert failOnUnimplemented abort into true");

        StatementBlock tonBlock;
        tonBlock.statements = {
            make_stmt(kThisOrNextFlag | 9999U, {reg(4)}),
            make_stmt(core_op::kAssign, {reg(2), 77}),
        };
        expect(!execute_statement_block(tonBlock, ctx) && ctx.registers[2] == 0 &&
                   ctx.unimplementedOpcodeCounts[9999U] == 1U && ctx.errorCount == 3,
               "this_or_next flag cannot swallow loud unimplemented WSE abort");

        wse::game::ScriptManager mgr;
        wse::game::Script callee;
        callee.id = "missing_callee";
        callee.block.statements = {make_stmt(83U, {reg(6)})};  // unused native slot 83
        mgr.scripts.push_back(std::move(callee));
        ctx.scripts = &mgr;

        StatementBlock callBlock;
        callBlock.statements = {
            make_stmt(core_op::kTryBegin, {}),
            make_stmt(core_op::kCallScript, {0}),
            make_stmt(core_op::kElseTry, {}),
            make_stmt(core_op::kAssign, {reg(3), 555}),
            make_stmt(core_op::kTryEnd, {}),
        };
        expect(!execute_statement_block(callBlock, ctx) && ctx.registers[3] == 0 &&
                   ctx.unimplementedOpcodeCounts[83U] == 1U && ctx.errorCount == 4,
               "failOnUnimplemented inside call_script aborts outer try/else caller");
    }
}

void test_native_entity_slots() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.world.has_game = true;
    ctx.world.troops.resize(1);
    ctx.world.parties.resize(1);
    ctx.world.factions.resize(1);
    ctx.world.sites.resize(1);
    ctx.world.party_templates.resize(1);
    ctx.world.agents.resize(1);
    ctx.world.quests.resize(1);
    ctx.world.items.resize(1);
    ctx.world.players.resize(1);
    ctx.world.mission_objects.resize(1);

    StatementBlock block;
    block.statements.push_back(make_stmt(core_op::kAssign, {reg(1), 0}));
    for (std::uint32_t kind = 0; kind < 11U; ++kind) {
        const std::int64_t val = 100LL + static_cast<std::int64_t>(kind);
        block.statements.push_back(make_stmt(core_op::kTroopSetSlot + kind, {0, 7, val}));
        block.statements.push_back(make_stmt(core_op::kTroopGetSlot + kind, {reg(0), 0, 7}));
        block.statements.push_back(make_stmt(core_op::kEq, {reg(0), val}));
        block.statements.push_back(make_stmt(core_op::kTroopSlotEq + kind, {0, 7, val}));
        block.statements.push_back(make_stmt(core_op::kTroopSlotGe + kind, {0, 7, val}));
        block.statements.push_back(make_stmt(core_op::kTryBegin, {}));
        block.statements.push_back(make_stmt(core_op::kTroopSlotEq + kind, {0, 7, val + 1}));
        block.statements.push_back(make_stmt(core_op::kElseTry, {}));
        block.statements.push_back(make_stmt(core_op::kValAdd, {reg(1), 1}));
        block.statements.push_back(make_stmt(core_op::kTryEnd, {}));
        block.statements.push_back(make_stmt(core_op::kTryBegin, {}));
        block.statements.push_back(make_stmt(core_op::kTroopSlotGe + kind, {0, 7, val + 1}));
        block.statements.push_back(make_stmt(core_op::kElseTry, {}));
        block.statements.push_back(make_stmt(core_op::kValAdd, {reg(1), 1}));
        block.statements.push_back(make_stmt(core_op::kTryEnd, {}));
    }

    // Invalid entity / out-of-range slot: *_set_slot no-ops, *_get_slot writes 0, eq/ge evaluate false.
    block.statements.push_back(make_stmt(core_op::kAssign, {reg(2), 999}));
    block.statements.push_back(make_stmt(core_op::kAssign, {reg(3), 888}));
    block.statements.push_back(make_stmt(core_op::kTroopSetSlot, {0, -1, 55}));
    block.statements.push_back(make_stmt(core_op::kPartySetSlot, {-1, 0, 55}));
    block.statements.push_back(make_stmt(core_op::kTroopGetSlot, {reg(2), 0, -1}));
    block.statements.push_back(make_stmt(core_op::kPartyGetSlot, {reg(3), 99, 0}));
    block.statements.push_back(make_stmt(core_op::kTryBegin, {}));
    block.statements.push_back(make_stmt(core_op::kTroopSlotEq, {0, wse::game::kMaxSlotIndex + 1, 0}));
    block.statements.push_back(make_stmt(core_op::kAssign, {reg(4), 1}));
    block.statements.push_back(make_stmt(core_op::kElseTry, {}));
    block.statements.push_back(make_stmt(core_op::kAssign, {reg(4), 2}));
    block.statements.push_back(make_stmt(core_op::kTryEnd, {}));

    expect(execute_statement_block(block, ctx) && ctx.errorCount == 0 &&
               ctx.unimplementedCount == 0 && ctx.registers[1] == 22 &&
               ctx.registers[2] == 0 && ctx.registers[3] == 0 && ctx.registers[4] == 2,
           "all 44 native entity_slots opcodes read/write/compare and zero-fill invalid lookups");
}

void test_native_arithmetic_registers_and_vars() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.randSeed = 42;
    ctx.fixedPointMultiplier = 100;
    ctx.world.has_game = true;
    ctx.world.troops.resize(1);
    ctx.world.troops[0].gold = 1234;

    StatementBlock block;
    block.statements = {
        make_stmt(core_op::kAssign, {reg(0), 10}),
        make_stmt(core_op::kAssign, {reg(1), 20}),
        make_stmt(core_op::kAssign, {reg(2), 30}),
        make_stmt(core_op::kAssign, {reg(3), 40}),
        make_stmt(core_op::kAssign, {reg(4), 50}),
        make_stmt(core_op::kAssign, {reg(5), 999}),
        make_stmt(core_op::kShuffleRange, {0, 5}),
        make_stmt(core_op::kShuffleRange, {4, 2}),  // degenerate no-op
        make_stmt(core_op::kStoreRandom, {reg(10), 10}),
        make_stmt(core_op::kStoreRandom, {reg(11), 1}),  // <=1 -> 0
        make_stmt(core_op::kStoreRandomInRange, {reg(12), -5, 5}),
        make_stmt(core_op::kStoreRandomInRange, {reg(13), 7, 7}),  // empty range -> lower
        make_stmt(core_op::kStoreAsin, {reg(14), 100}),
        make_stmt(core_op::kStoreAcos, {reg(15), 0}),
        make_stmt(core_op::kStoreAtan, {reg(16), 100}),
        make_stmt(core_op::kStoreAtan2, {reg(17), 100, -100}),
        make_stmt(core_op::kStoreTroopGold, {reg(18), 0}),
    };

    const bool ok = execute_statement_block(block, ctx);
    std::array<std::int64_t, 5> shuffled{ctx.registers[0], ctx.registers[1], ctx.registers[2],
                                         ctx.registers[3], ctx.registers[4]};
    std::sort(shuffled.begin(), shuffled.end());
    const std::array<std::int64_t, 5> expectedShuffled{10, 20, 30, 40, 50};
    expect(ok && ctx.errorCount == 0 && ctx.unimplementedCount == 0 &&
               shuffled == expectedShuffled && ctx.registers[5] == 999 &&
               ctx.registers[10] >= 0 && ctx.registers[10] < 10 && ctx.registers[11] == 0 &&
               ctx.registers[12] >= -5 && ctx.registers[12] < 5 && ctx.registers[13] == 7 &&
               std::abs(static_cast<double>(ctx.registers[14] - 9000)) <= 2.0 &&
               std::abs(static_cast<double>(ctx.registers[15] - 9000)) <= 2.0 &&
               std::abs(static_cast<double>(ctx.registers[16] - 4500)) <= 2.0 &&
               std::abs(static_cast<double>(ctx.registers[17] - 13500)) <= 2.0 &&
               ctx.registers[18] == 1234,
           "shuffle_range, store_random*, trig stores, and store_troop_gold match v1.1.5.6");
}

void test_native_positions_and_math() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.fixedPointMultiplier = 100;

    StatementBlock block;
    block.statements = {
        make_stmt(core_op::kInitPosition, {0}),
        make_stmt(core_op::kPositionSetX, {0, 300}),
        make_stmt(core_op::kPositionSetY, {0, 400}),
        make_stmt(core_op::kPositionSetZ, {0, 0}),
        make_stmt(core_op::kPositionGetX, {reg(0), 0}),
        make_stmt(core_op::kPositionGetY, {reg(1), 0}),
        make_stmt(core_op::kPositionGetZ, {reg(2), 0}),

        make_stmt(core_op::kInitPosition, {1}),
        make_stmt(core_op::kGetDistanceBetweenPositions, {reg(3), 0, 1}),
        make_stmt(core_op::kGetDistanceBetweenPositionsInMeters, {reg(4), 0, 1}),

        make_stmt(core_op::kCopyPosition, {2, 0}),
        make_stmt(core_op::kPositionNormalizeOrigin, {reg(5), 2}),
        make_stmt(core_op::kPositionGetX, {reg(6), 2}),
        make_stmt(core_op::kPositionGetY, {reg(7), 2}),

        make_stmt(core_op::kInitPosition, {3}),
        make_stmt(core_op::kPositionCopyOrigin, {3, 0}),
        make_stmt(core_op::kPositionSetScaleX, {3, 150}),
        make_stmt(core_op::kPositionSetScaleY, {3, 250}),
        make_stmt(core_op::kPositionSetScaleZ, {3, 350}),
        make_stmt(core_op::kPositionGetScaleX, {reg(8), 3}),
        make_stmt(core_op::kPositionGetScaleY, {reg(9), 3}),
        make_stmt(core_op::kPositionGetScaleZ, {reg(10), 3}),

        // Local vs global moves and rotations on pos4/pos5.
        make_stmt(core_op::kInitPosition, {4}),
        make_stmt(core_op::kPositionRotateZ, {4, 90, 0}),
        make_stmt(core_op::kPositionMoveY, {4, 200, 0}),   // +2m along rotated local Y
        make_stmt(core_op::kPositionMoveX, {4, 100, 1}),   // +1m world X
        make_stmt(core_op::kPositionMoveZ, {4, 50, 1}),    // +0.5m world Z
        make_stmt(core_op::kPositionRotateZ, {4, -90, 0}),
        make_stmt(core_op::kPositionRotateX, {4, 30, 0}),
        make_stmt(core_op::kPositionRotateY, {4, 20, 0}),
        make_stmt(core_op::kPositionRotateXFloating, {4, -3000, 0}),
        make_stmt(core_op::kPositionRotateYFloating, {4, -2000, 0}),
        make_stmt(core_op::kPositionRotateZFloating, {4, 9000, 0}),
        make_stmt(core_op::kPositionGetRotationAroundX, {reg(11), 4}),
        make_stmt(core_op::kPositionGetRotationAroundY, {reg(12), 4}),
        make_stmt(core_op::kPositionGetRotationAroundZ, {reg(13), 4}),
        make_stmt(core_op::kInitPosition, {5}),
        make_stmt(core_op::kPositionCopyRotation, {5, 4}),

        // Transform to parent & back to local round-trip.
        make_stmt(core_op::kInitPosition, {6}),
        make_stmt(core_op::kPositionSetX, {6, 100}),
        make_stmt(core_op::kPositionTransformPositionToParent, {7, 4, 6}),
        make_stmt(core_op::kPositionTransformPositionToLocal, {8, 4, 7}),
        make_stmt(core_op::kPositionGetX, {reg(14), 8}),
        make_stmt(core_op::kPositionGetY, {reg(15), 8}),

        // Behind check and angle between positions.
        make_stmt(core_op::kInitPosition, {9}),
        make_stmt(core_op::kInitPosition, {10}),
        make_stmt(core_op::kPositionSetY, {10, -200}),
        make_stmt(core_op::kPositionIsBehindPosition, {10, 9}),
        make_stmt(core_op::kInitPosition, {11}),
        make_stmt(core_op::kPositionSetY, {11, 200}),
        make_stmt(kNegFlag | core_op::kPositionIsBehindPosition, {11, 9}),
        make_stmt(core_op::kGetAngleBetweenPositions, {reg(16), 9, 11}),
    };

    expect(execute_statement_block(block, ctx) && ctx.errorCount == 0 &&
               ctx.unimplementedCount == 0 &&
               ctx.registers[0] == 300 && ctx.registers[1] == 400 && ctx.registers[2] == 0 &&
               ctx.registers[3] == 500 && ctx.registers[4] == 5 &&
               ctx.registers[5] == 500 && ctx.registers[6] == 60 && ctx.registers[7] == 80 &&
               ctx.registers[8] == 150 && ctx.registers[9] == 250 && ctx.registers[10] == 350 &&
               ctx.registers[11] >= 0 && ctx.registers[11] < 360 &&
               ctx.registers[12] >= 0 && ctx.registers[12] < 360 &&
               ctx.registers[13] >= 0 && ctx.registers[13] < 360 &&
               std::abs(static_cast<double>(ctx.registers[14] - 100)) <= 2.0 &&
               std::abs(static_cast<double>(ctx.registers[15])) <= 2.0 &&
               ctx.positions[5].f.y == ctx.positions[4].f.y &&
               std::abs(static_cast<double>(ctx.registers[16])) <= 2,
           "all 35 positions_and_math opcodes execute and satisfy geometric invariants");
}

void test_native_strings_and_entity_ops() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.world.has_game = true;
    ctx.world.skills[wse::game::kSkillIronflesh].max_level = 10;

    ctx.world.troops.resize(2);
    ctx.world.troops[0].flags = 0x10U;
    ctx.world.troops[0].attributes[0] = 15;
    ctx.world.troops[0].set_skill_raw(wse::game::kSkillIronflesh, 5);  // max_hp = 15+35+10 = 60
    ctx.world.troops[0].name = "Knight";
    ctx.world.troops[0].plural_name = "Knights";
    ctx.world.troops[0].faction_no = 2;
    ctx.world.troops[1].flags = 0U;

    ctx.world.items.resize(1);
    ctx.world.items[0].name = "Blade";
    ctx.world.items[0].plural_name = "Blades";

    ctx.world.parties.resize(1);
    ctx.world.parties[0].name = "Scouts";
    ctx.world.parties[0].faction_no = 4;

    ctx.world.factions.resize(3);
    ctx.world.factions[2].name = "Swadia";

    ctx.world.quests.resize(1);
    ctx.world.quests[0].title = "Aid {reg1} towns";

    ctx.world.players.resize(1);
    ctx.world.players[0].status = 2;
    ctx.world.players[0].name = "Commander";

    ctx.world.agents.resize(2);
    ctx.world.agents[0].type = 0;
    ctx.world.agents[1].type = 1;

    ctx.stringRegisters[10] = u"hello {reg1}!";
    ctx.stringRegisters[11] = u"hero world!";

    StatementBlock block;
    block.statements = {
        make_stmt(core_op::kAssign, {reg(1), 7}),
        make_stmt(core_op::kTroopIsHero, {0}),
        make_stmt(kNegFlag | core_op::kTroopIsHero, {1}),
        make_stmt(core_op::kTroopSetHealth, {0, 50, 0}),  // 50% of 60 -> 30
        make_stmt(core_op::kAgentIsHuman, {0}),
        make_stmt(kNegFlag | core_op::kAgentIsHuman, {1}),
        make_stmt(core_op::kStoreTroopFaction, {reg(2), 0}),
        make_stmt(core_op::kStoreFactionOfParty, {reg(3), 0}),

        make_stmt(core_op::kStrStoreString, {sreg(0), sreg(10)}),
        make_stmt(core_op::kStrStoreStringReg, {sreg(1), sreg(10)}),
        make_stmt(core_op::kDisplayMessage, {sreg(0), 0}),
        make_stmt(core_op::kDisplayMessage, {sreg(0), 0x00ff00ff}),
        make_stmt(core_op::kStrClear, {sreg(10)}),
        make_stmt(core_op::kStrIsEmpty, {sreg(10)}),
        make_stmt(kNegFlag | core_op::kStrIsEmpty, {sreg(0)}),

        make_stmt(core_op::kStrStoreTroopName, {sreg(2), 0}),
        make_stmt(core_op::kStrStoreTroopNamePlural, {sreg(3), 0}),
        make_stmt(core_op::kStrStoreTroopNameByCount, {sreg(4), 0, 1}),
        make_stmt(core_op::kStrStoreTroopNameByCount, {sreg(5), 0, 2}),

        make_stmt(core_op::kStrStoreItemName, {sreg(6), 0}),
        make_stmt(core_op::kStrStoreItemNamePlural, {sreg(7), 0}),
        make_stmt(core_op::kStrStoreItemNameByCount, {sreg(8), 0, 1}),
        make_stmt(core_op::kStrStoreItemNameByCount, {sreg(9), 0, 3}),

        make_stmt(core_op::kStrStorePartyName, {sreg(12), 0}),
        make_stmt(core_op::kStrStoreFactionName, {sreg(13), 2}),
        make_stmt(core_op::kStrStoreQuestName, {sreg(14), 0}),
        make_stmt(core_op::kStrStoreTroopNameLink, {sreg(15), 0}),
        make_stmt(core_op::kStrStorePartyNameLink, {sreg(16), 0}),
        make_stmt(core_op::kStrStoreFactionNameLink, {sreg(17), 2}),
        make_stmt(core_op::kStrStoreQuestNameLink, {sreg(18), 0}),
        make_stmt(core_op::kStrStorePlayerUsername, {sreg(19), 0}),
        make_stmt(core_op::kStrEncodeUrl, {sreg(11)}),
    };

    const bool ok = execute_statement_block(block, ctx);
    StatementBlock clampHpBlock;
    clampHpBlock.statements = {make_stmt(core_op::kTroopSetHealth, {0, 999, 1})};
    const float halfHp = ctx.world.troops[0].hit_points;
    const bool ok2 = execute_statement_block(clampHpBlock, ctx);

    expect(ok && ok2 && ctx.errorCount == 0 && ctx.unimplementedCount == 0 &&
               halfHp == 30.0f && ctx.world.troops[0].hit_points == 60.0f &&
               ctx.registers[2] == 2 && ctx.registers[3] == 4 &&
               ctx.world.displayed_messages.size() == 2U &&
               ctx.world.displayed_messages[0].text == u"hello 7!" &&
               ctx.world.displayed_messages[0].color == 0xffffffffu &&
               ctx.world.displayed_messages[1].color == 0x00ff00ffu &&
               ctx.stringRegisters[0] == u"hello 7!" && ctx.stringRegisters[1] == u"hello 7!" &&
               ctx.stringRegisters[10].empty() &&
               ctx.stringRegisters[2] == u"Knight" && ctx.stringRegisters[3] == u"Knights" &&
               ctx.stringRegisters[4] == u"Knight" && ctx.stringRegisters[5] == u"Knights" &&
               ctx.stringRegisters[6] == u"Blade" && ctx.stringRegisters[7] == u"Blades" &&
               ctx.stringRegisters[8] == u"Blade" && ctx.stringRegisters[9] == u"Blades" &&
               ctx.stringRegisters[12] == u"Scouts" && ctx.stringRegisters[13] == u"Swadia" &&
               ctx.stringRegisters[14] == u"Aid 7 towns" &&
               ctx.stringRegisters[15] == u"<trp=0>Knight</>" &&
               ctx.stringRegisters[16] == u"<p=0>Scouts</>" &&
               ctx.stringRegisters[17] == u"<fac=2>Swadia</>" &&
               ctx.stringRegisters[18] == u"<qst=0>Aid {reg1} towns</>" &&
               ctx.stringRegisters[19] == u"Commander" &&
               ctx.stringRegisters[11] == u"hero%20world%21",
           "all 25 strings/UI/entity native opcodes match v1.1.5.6 semantics");
}

void test_entity_iterators() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.fixedPointMultiplier = 100;
    ctx.world.has_game = true;

    // 3 parties: 0 (active), 1 (inactive), 2 (active).
    ctx.world.parties.resize(3);
    ctx.world.parties[0].active = true;
    ctx.world.parties[1].active = false;
    ctx.world.parties[2].active = true;

    // 3 agents: 0 (active, alive, near), 1 (inactive), 2 (active, wounded, far).
    ctx.world.agents.resize(3);
    ctx.world.agents[0].active = true;
    ctx.world.agents[0].status = 1;
    ctx.world.agents[0].position.o = {1.0f, 1.0f, 0.0f, 0.0f};
    ctx.world.agents[1].active = false;
    ctx.world.agents[2].active = true;
    ctx.world.agents[2].status = 3;
    ctx.world.agents[2].position.o = {20.0f, 0.0f, 0.0f, 0.0f};

    // 4 mission objects for try_for_prop_instances filters.
    ctx.world.mission_objects.resize(4);
    ctx.world.mission_objects[0] = {};
    ctx.world.mission_objects[0].active = true;
    ctx.world.mission_objects[0].scene_prop_no = 10;
    ctx.world.mission_objects[0].meta_mesh_no = 2;
    ctx.world.mission_objects[1].active = false;
    ctx.world.mission_objects[1].scene_prop_no = 10;
    ctx.world.mission_objects[1].meta_mesh_no = 2;
    ctx.world.mission_objects[2].active = true;
    ctx.world.mission_objects[2].scene_prop_no = 20;
    ctx.world.mission_objects[2].meta_mesh_no = 2;
    ctx.world.mission_objects[3].active = true;
    ctx.world.mission_objects[3].scene_prop_no = 10;
    ctx.world.mission_objects[3].meta_mesh_no = 5;

    // 4 players: 0 (server active), 1 (inactive), 2 (active), 3 (active).
    ctx.world.max_players = 4;
    ctx.world.players.resize(4);
    ctx.world.players[0].status = 2;
    ctx.world.players[1].status = 0;
    ctx.world.players[2].status = 2;
    ctx.world.players[3].status = 2;

    StatementBlock block;
    block.statements = {
        // Party loop (0, 2 -> encoded bitmask in reg0 = (1<<0)|(1<<2) = 5, with continue skipping id==99 none)
        make_stmt(core_op::kAssign, {reg(0), 0}),
        make_stmt(core_op::kTryForParties, {loc(0)}),
        make_stmt(core_op::kValAdd, {loc(0), 1}),
        make_stmt(core_op::kStoreAdd, {reg(0), reg(0), loc(0)}),
        make_stmt(core_op::kTryEnd, {}),

        // Agent whole-world loop (0, 2 -> count in reg1 = 2, sum+1 in reg2 = 4)
        make_stmt(core_op::kAssign, {reg(1), 0}),
        make_stmt(core_op::kAssign, {reg(2), 0}),
        make_stmt(core_op::kTryForAgents, {loc(1)}),
        make_stmt(core_op::kValAdd, {reg(1), 1}),
        make_stmt(core_op::kValAdd, {loc(1), 1}),
        make_stmt(core_op::kValAdd, {reg(2), loc(1)}),
        make_stmt(core_op::kTryEnd, {}),

        // Agent distance-filtered loop around pos0=(0,0,0) radius 500 (5.0m) -> only agent 0!
        make_stmt(core_op::kInitPosition, {0}),
        make_stmt(core_op::kAssign, {reg(3), 0}),
        make_stmt(core_op::kTryForAgents, {loc(2), 0, 500}),
        make_stmt(core_op::kValAdd, {reg(3), 1}),
        make_stmt(core_op::kAssign, {reg(4), loc(2)}),
        make_stmt(core_op::kTryEnd, {}),

        // Prop instances unfiltered (0, 2, 3 -> count=3 in reg5), prop=10 (0, 3 -> count=2 in reg6),
        // prop=10 & meta+1=3 (meta==2 -> only 0 -> count=1 in reg7).
        make_stmt(core_op::kAssign, {reg(5), 0}),
        make_stmt(core_op::kTryForPropInstances, {loc(3)}),
        make_stmt(core_op::kValAdd, {reg(5), 1}),
        make_stmt(core_op::kTryEnd, {}),

        make_stmt(core_op::kAssign, {reg(6), 0}),
        make_stmt(core_op::kTryForPropInstances, {loc(3), 10}),
        make_stmt(core_op::kValAdd, {reg(6), 1}),
        make_stmt(core_op::kTryEnd, {}),

        make_stmt(core_op::kAssign, {reg(7), 0}),
        make_stmt(core_op::kTryForPropInstances, {loc(3), 10, 3}),
        make_stmt(core_op::kValAdd, {reg(7), 1}),
        make_stmt(core_op::kAssign, {reg(8), loc(3)}),
        make_stmt(core_op::kTryEnd, {}),

        // Players mode 0 (0, 2, 3 -> count=3 in reg9) and mode 1 skipping server (2, 3 -> count=2 in reg10)
        // plus nested break_loop on player == 2 in reg11 (visited only player 0 -> sum+1 = 1).
        make_stmt(core_op::kAssign, {reg(9), 0}),
        make_stmt(core_op::kTryForPlayers, {loc(4), 0}),
        make_stmt(core_op::kValAdd, {reg(9), 1}),
        make_stmt(core_op::kTryEnd, {}),

        make_stmt(core_op::kAssign, {reg(10), 0}),
        make_stmt(core_op::kTryForPlayers, {loc(4), 1}),
        make_stmt(core_op::kValAdd, {reg(10), 1}),
        make_stmt(core_op::kTryEnd, {}),

        make_stmt(core_op::kAssign, {reg(11), 0}),
        make_stmt(core_op::kTryForPlayers, {loc(4)}),
        make_stmt(core_op::kTryBegin, {}),
        make_stmt(core_op::kEq, {loc(4), 2}),
        make_stmt(core_op::kBreakLoop, {}),
        make_stmt(core_op::kTryEnd, {}),
        make_stmt(core_op::kValAdd, {reg(11), 1}),
        make_stmt(core_op::kTryEnd, {}),
    };

    const bool ok = execute_statement_block(block, ctx);
    expect(ok && ctx.errorCount == 0 && ctx.unimplementedCount == 0 &&
               ctx.registers[0] == 4 && ctx.registers[1] == 2 && ctx.registers[2] == 4 &&
               ctx.registers[3] == 1 && ctx.registers[4] == 0 &&
               ctx.registers[5] == 3 && ctx.registers[6] == 2 &&
               ctx.registers[7] == 1 && ctx.registers[8] == 0 &&
               ctx.registers[9] == 3 && ctx.registers[10] == 2 && ctx.registers[11] == 1,
           "all 4 entity iterators (11, 12, 16, 17) iterate, filter, and break accurately");
}

void test_native_entity_ops() {
    using namespace wse::vm;
    namespace game = wse::game;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.fixedPointMultiplier = 100;
    ctx.currentCaller = 42;
    auto& w = ctx.world;
    w.has_game = true;
    w.game_year = 1257;
    w.current_day = 42;
    w.ground_level_z = 3.25f;
    w.num_skins = 3;
    w.attribute_limit = 63;
    w.conversation_partner_troop_no = 1;
    w.partner_faction_no = 2;
    w.main_party_no = 0;
    w.current_hours = 777;
    w.skills[game::kSkillIronflesh].max_level = 10;
    w.skills[game::kSkillInventoryManagement].max_level = 10;
    w.skills[26].max_level = 10;

    w.max_players = 4;
    w.players.resize(3);
    w.players[0].status = 2;
    w.players[0].is_admin = true;
    w.players[0].agent_no = 99;  // invalid -> cleared to -1 by 406
    w.players[1].status = 2;
    w.players[1].agent_no = 0;
    w.players[2].status = 0;

    w.agents.resize(1);
    w.agents[0].active = true;

    w.factions.resize(3);
    w.quests.resize(3);
    w.quests[1].template_flags = 2U;  // repeatable
    w.sites.resize(2);

    w.items.resize(3);
    w.items[0].value = 100;
    w.items[0].max_ammo = 30;
    w.items[0].properties = 0ULL;
    w.items[1].value = 250;
    w.items[1].max_ammo = 1;
    w.items[1].properties = (1ULL << 13);  // unique/always-lootable flag for 1539
    w.items[2].value = 50;
    w.items[2].max_ammo = 10;

    w.troops.resize(3);
    w.troops[0].flags = 0x10U;  // hero
    w.troops[0].set_skill_raw(game::kSkillInventoryManagement, 2);  // raised to 3 -> 30 + 3*6 + 10 = 58
    w.troops[0].set_skill_raw(26, 4);
    w.troops[0].attributes = {10, 11, 12, 13};
    w.troops[0].level = 14;
    w.troops[0].faction_no = 1;
    w.troops[0].hit_points = 45.0f;
    w.troops[0].gold = 100;
    w.troops[0].inventory[game::kNumTroopEquipmentSlots] = game::WseInventorySlot{0, 12, 25, 0};
    w.troops[1].faction_no = 2;
    w.troops[1].inventory[5] = game::WseInventorySlot{1, 1, 0, 1};

    w.parties.resize(3);
    w.parties[0].active = true;
    w.parties[0].current_terrain = 3;
    w.parties[0].battle_opponent_no = 1;
    w.parties[0].position.o = {0.0f, 0.0f, 0.0f, 0.0f};
    w.parties[0].stacks = {game::WsePartyStack{0, 1, 0, 0}, game::WsePartyStack{1, 4, 0, 1}};
    w.parties[1].active = true;
    w.parties[1].position.o = {3.0f, 4.0f, 0.0f, 0.0f};  // distance = 5.0
    w.parties[2].active = true;

    ctx.stringRegisters[0] = u"hello player";
    ctx.stringRegisters[1] = u"Quest note";
    ctx.stringRegisters[2] = u"NewOverlay";
    ctx.stringRegisters[3] = u"RenamedWarband";

    // Player/MP (47, 394-408, 430), Quests (200-204, 1113, 1280-1284, 1286),
    // Audio/UI (599, 600, 791, 900-903, 910, 920-922, 925-927, 947, 1107),
    // Sites/Factions (1232, 1261-1264, 1270).
    StatementBlock blockA;
    blockA.statements = {
        make_stmt(47, {1}),
        make_stmt(394, {0, 10}),
        make_stmt(395, {0, 11, 1}),
        make_stmt(396, {0, 12, 1, 2}),
        make_stmt(397, {0, 13, 1, 2, 3}),
        make_stmt(398, {0, 14, 1, 2, 3, 4}),
        make_stmt(399, {0, 15, sreg(0)}),
        make_stmt(400, {reg(38)}),
        make_stmt(401, {0}),
        make_stmt(kNegFlag | 401, {2}),
        make_stmt(403, {1, 2}),
        make_stmt(402, {reg(1), 1}),
        make_stmt(405, {1, 2}),
        make_stmt(404, {reg(2), 1}),
        make_stmt(406, {reg(3), 0}),
        make_stmt(406, {reg(4), 1}),
        make_stmt(408, {1, 800, 500}),
        make_stmt(407, {reg(5), 1}),
        make_stmt(430, {0}),
        make_stmt(kNegFlag | 430, {1}),

        // Quests 200-204, 1113, 1280-1284, 1286
        make_stmt(1280, {0, 2}),
        make_stmt(200, {0}),
        make_stmt(1113, {0, 0, sreg(1), 1}),
        make_stmt(1282, {0}),
        make_stmt(202, {0}),
        make_stmt(1281, {0}),
        make_stmt(201, {0}),
        make_stmt(1286, {0}),
        make_stmt(204, {0}),
        make_stmt(1283, {2}),
        make_stmt(203, {2}),
        make_stmt(1284, {2}),
        make_stmt(kNegFlag | 200, {2}),
        make_stmt(1280, {1, 0}),
        make_stmt(1281, {1}),
        make_stmt(kNegFlag | 200, {1}),

        // Audio / ground / presentations / overlays
        make_stmt(core_op::kInitPosition, {0}),
        make_stmt(core_op::kPositionSetX, {0, 150}),
        make_stmt(core_op::kPositionSetY, {0, 250}),
        make_stmt(599, {5, 0, 2}),
        make_stmt(600, {6, 1}),
        make_stmt(791, {0}),
        make_stmt(900, {3}),
        make_stmt(903, {3}),
        make_stmt(902, {120, 3}),
        make_stmt(910, {reg(6), sreg(2), 7}),
        make_stmt(920, {reg(6), sreg(3)}),
        make_stmt(921, {reg(6), 0x11223344}),
        make_stmt(922, {reg(6), 128}),
        make_stmt(925, {reg(6), 0}),
        make_stmt(926, {reg(6), 0}),
        make_stmt(927, {reg(6), 99}),
        make_stmt(947, {reg(6), 0}),
        make_stmt(1107, {0}),

        // Sites & factions
        make_stmt(1232, {2}),
        make_stmt(1261, {1}),
        make_stmt(1263, {5, 1, 2}),
        make_stmt(1264, {6, 0, 2}),
        make_stmt(1263, {5, -1, 0}),  // deletes entry 5, leaving entry 6 x2
        make_stmt(1262, {}),          // resets visitors -> clear before re-adding
        make_stmt(1263, {4, 1, 9}),
        make_stmt(1270, {0, 2, 50}),
    };

    // Test 901 caller == 8 vs caller == 0 and preservation of ctx.currentCaller == 42.
    StatementBlock presCallerBlock;
    presCallerBlock.statements = {make_stmt(901, {7})};
    StatementBlock presNonCallerBlock;
    presNonCallerBlock.statements = {make_stmt(901, {9})};

    // Troops (1505-1550, 2170-2176) & Parties (1601-1685, 2156-2158, 2201, 2270, 2281).
    StatementBlock blockB;
    blockB.statements = {
        make_stmt(1505, {0, 2}),
        make_stmt(1506, {reg(7), 0}),
        make_stmt(1509, {0, 0}),
        make_stmt(1517, {0, 4}),
        make_stmt(1516, {reg(8), 0}),
        make_stmt(1520, {0, 0, 3}),
        make_stmt(2172, {reg(9), 0, 0}),
        make_stmt(1521, {0, game::kSkillInventoryManagement, 1}),
        make_stmt(2170, {reg(10), game::kSkillInventoryManagement, 0}),
        make_stmt(2171, {reg(11), 0}),
        make_stmt(1522, {0, 0, 50}),
        make_stmt(1523, {0, 1, 25}),
        make_stmt(2176, {reg(12), 0, 1}),
        make_stmt(1528, {0, 50}),
        make_stmt(1529, {0, 20}),
        make_stmt(1537, {reg(13), 0, game::kNumTroopEquipmentSlots}),
        make_stmt(1538, {reg(14), 0, game::kNumTroopEquipmentSlots}),
        make_stmt(1540, {reg(15), 0}),
        make_stmt(1541, {reg(16), 0, game::kNumTroopEquipmentSlots}),
        make_stmt(1542, {reg(17), 0, game::kNumTroopEquipmentSlots}),
        make_stmt(1530, {0, 2, 10}),
        make_stmt(1535, {0, 0, 2}),
        make_stmt(1536, {0, 0, 2}),  // removes 2x item 0 (value 100 each) -> reg0 = 200
        make_stmt(core_op::kAssign, {reg(18), reg(0)}),
        make_stmt(1532, {0}),
        make_stmt(1539, {0, 1, 100}),  // guaranteed copies item 1 into slot 10
        make_stmt(1550, {0, 2}),
        make_stmt(2175, {reg(19), 0, 1}),  // raw HP = 45 + 3 (from str +3) = 48

        // Parties
        make_stmt(1610, {0, 2, 3}),
        make_stmt(1601, {reg(20), 0}),  // 1 (leader) + 3 = 4 companions
        make_stmt(1602, {reg(21), 0}),  // 4 prisoners
        make_stmt(1603, {0, 0x20, 1}),
        make_stmt(1603, {0, 0x10, 0}),
        make_stmt(1608, {reg(22), 0}),
        make_stmt(1615, {0, 2, 2}),
        make_stmt(1620, {0, 1}),
        make_stmt(core_op::kInitPosition, {1}),
        make_stmt(core_op::kPositionSetX, {1, 600}),
        make_stmt(1626, {1, 1}),
        make_stmt(1625, {2, 1}),
        make_stmt(core_op::kPositionGetX, {reg(23), 2}),
        make_stmt(1640, {0, 8}),
        make_stmt(1641, {0, 1}),
        make_stmt(1665, {reg(24), 0}),  // behavior==8 -> returns 1
        make_stmt(1650, {reg(25), 0}),
        make_stmt(1651, {reg(26), 0}),
        make_stmt(1652, {reg(27), 0, 0}),
        make_stmt(1653, {reg(28), 0, 0}),
        make_stmt(1660, {0, 1}),
        make_stmt(1661, {0}),
        make_stmt(1669, {0, sreg(3)}),
        make_stmt(1676, {0, 19}),
        make_stmt(1677, {0, 21}),
        make_stmt(1681, {reg(29), 0}),
        make_stmt(1680, {reg(30), 0}),
        make_stmt(1685, {reg(31), 0, 26}),
        make_stmt(2156, {reg(32), 0}),
        make_stmt(2157, {reg(33), 0}),
        make_stmt(2158, {reg(34), 2}),  // remaining count of troop 2 in main party = 1
        make_stmt(2201, {reg(35)}),
        make_stmt(2270, {reg(36)}),
        make_stmt(2281, {reg(37), 0, 1}),  // distance from (0,0,0) to (6,0,0) = 6
    };

    const bool okA = execute_statement_block(blockA, 0, 0, {}, ctx);
    const bool okNonCaller = execute_statement_block(presNonCallerBlock, 0, 0, {}, ctx);
    const bool ignoredByZeroCaller = (w.active_presentation_no == 3);
    const bool okCaller = execute_statement_block(presCallerBlock, 8, 0, {}, ctx);
    const bool updatedByCaller8 = (w.active_presentation_no == 7 && w.presentation_background);
    const bool callerRestored = (ctx.currentCaller == 42);
    const bool okB = execute_statement_block(blockB, 0, 0, {}, ctx);

    expect(okA && okNonCaller && ignoredByZeroCaller && okCaller && updatedByCaller8 &&
               callerRestored && okB && ctx.errorCount == 0 && ctx.unimplementedCount == 0 &&
               w.player_troop_no == 1 && w.multiplayer_messages.size() == 6U &&
               ctx.registers[38] == 4 && ctx.registers[0] == 2 &&
               ctx.registers[1] == 2 && ctx.registers[2] == 2 &&
               ctx.registers[3] == -1 && ctx.registers[4] == 0 && ctx.registers[5] == 500 &&
               w.quests[0].giver_troop_no == 2 && w.quests[0].notes[0].date == 42 &&
               w.played_sounds.size() == 2U && ctx.positions[0].o.z == 3.25f &&
               w.presentation_duration == 120 && !w.show_messages && w.overlays.size() == 1U &&
               w.overlays[0].text == u"RenamedWarband" && w.overlays[0].val == 99 &&
               !w.overlays[0].visible && !w.parties[2].active &&
               w.sites[1].visitors.size() == 1U && w.sites[1].visitors[0].dna == 9 &&
               std::abs(w.factions[0].relations[2] - 0.5f) < 0.001f &&
               ctx.registers[7] == 2 && !w.troops[0].auto_equip && ctx.registers[8] == 4 &&
               ctx.registers[9] == 13 && ctx.registers[10] == 3 && ctx.registers[11] == 14 &&
               ctx.registers[12] == 25 && w.troops[0].gold == 130 &&
               ctx.registers[13] == 25 && ctx.registers[14] == 30 && ctx.registers[15] == 58 &&
               ctx.registers[16] == 0 && ctx.registers[17] == 12 && ctx.registers[18] == 200 &&
               w.troops[0].inventory[game::kNumTroopEquipmentSlots].item_no == 1 &&
               ctx.registers[19] == 48 && ctx.registers[20] == 4 && ctx.registers[21] == 4 &&
               ctx.registers[22] == 3 && ctx.registers[23] == 600 && ctx.registers[24] == 1 &&
               ctx.registers[25] == 2 && ctx.registers[26] == 1 && ctx.registers[27] == 0 &&
               ctx.registers[28] == 1 && w.parties[0].name == "RenamedWarband" &&
               w.parties[0].banner_icon_no == 21 && ctx.registers[29] == 19 &&
               ctx.registers[30] == 1 && ctx.registers[31] == 4 && ctx.registers[32] == 6 &&
               ctx.registers[33] == 2 && ctx.registers[34] == 1 && ctx.registers[35] == 2 &&
               ctx.registers[36] == 777 && ctx.registers[37] == 6,
           "all 108 entity/player/quest/UI/site/troop/party native opcodes match v1.1.5.6");
}

void test_native_mission_ops() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    ctx.fixedPointMultiplier = 100;
    ctx.currentCaller = 99;
    ctx.timersTotal = {500000LL, 250000LL, 100000LL};
    auto& w = ctx.world;
    w.has_game = true;
    w.ground_level_z = 2.0f;
    w.player_agent_no = 0;
    w.main_party_side = 0;
    w.player_agent_own_troop_kill_count = 3;
    w.player_agent_own_troop_wound_count = 4;
    w.mission_timers[0] = {0, 500000LL};
    w.mission_timers[1] = {1, 50000LL};
    w.mission_timers[2] = {0, 0LL};
    w.mission_templates.resize(2);

    w.items.resize(2);
    w.items[0].max_ammo = 24;
    w.items[1].max_ammo = 8;

    w.teams[0].relations.fill(0);
    w.teams[0].relations[1] = 1;
    w.teams[1].relations.fill(0);
    w.teams[1].relations[0] = 1;
    w.teams[0].division_leaders[0] = 1;

    w.agents.resize(2);
    w.agents[0].active = true;
    w.agents[0].status = 1;
    w.agents[0].team_no = 0;
    w.agents[0].troop_no = 2;
    w.agents[0].party_no = 0;
    w.agents[0].horse_agent_no = 1;
    w.agents[0].hit_points = 80.0f;
    w.agents[0].max_hit_points = 100.0f;
    w.agents[0].kill_count = 5;
    w.agents[0].wound_count = 2;
    w.agents[0].equipped_items[0] = 0;
    w.agents[0].item_ammo[0] = 5;
    w.agents[0].equipped_items[1] = 1;
    w.agents[0].item_ammo[1] = 3;
    w.agents[0].wielded_items = {0, -1};
    w.agents[0].attached_mission_object_nos[0] = 77;
    w.agents[0].simple_behavior = 4;
    w.agents[0].class_no = 1;

    w.agents[1].active = true;
    w.agents[1].status = 3;  // wounded
    w.agents[1].is_non_player = true;
    w.agents[1].team_no = 1;
    w.agents[1].rider_agent_no = 0;

    w.entry_points[3].position.o = {4.0f, 5.0f, -150.0f, 0.0f};
    w.order_menu_open = true;

    w.mission_objects.resize(2);
    w.mission_objects[0].active = true;
    w.mission_objects[0].scene_prop_no = 15;
    w.mission_objects[0].hit_points = 50.0f;
    w.mission_objects[0].max_hit_points = 100.0f;
    w.mission_objects[1].active = true;
    w.mission_objects[1].scene_prop_no = 15;

    StatementBlock block;
    block.statements = {
        // Agents 1700-1804
        make_stmt(1700, {reg(0)}),
        make_stmt(1702, {0}),
        make_stmt(1703, {1}),
        make_stmt(1705, {reg(1), 0}),
        make_stmt(1705, {reg(2), 1}),
        make_stmt(1706, {1}),
        make_stmt(1707, {1}),
        make_stmt(1708, {1}),
        make_stmt(1712, {0}),
        make_stmt(core_op::kInitPosition, {0}),
        make_stmt(core_op::kPositionSetX, {0, 900}),
        make_stmt(1711, {0, 0}),
        make_stmt(1710, {1, 0}),
        make_stmt(core_op::kPositionGetX, {reg(3), 1}),
        make_stmt(1714, {reg(4), 0}),
        make_stmt(1715, {reg(5), 1}),
        make_stmt(1716, {reg(6), 0}),
        make_stmt(1718, {reg(7), 0}),
        make_stmt(1720, {reg(8), 0, 1}),
        make_stmt(1721, {0, 50, 0}),  // 50% of 100 -> 50
        make_stmt(1723, {reg(9), 0, 0}),
        make_stmt(1726, {reg(10), 0, 0}),
        make_stmt(1727, {reg(11), 0, 0}),  // total ammo 5+3=8
        make_stmt(1776, {0, 1, 20}),       // clamped to max_ammo=8
        make_stmt(1728, {0}),
        make_stmt(1727, {reg(12), 0, 1}),  // refilled wielded ammo = 24
        make_stmt(1729, {0, 1}),
        make_stmt(1747, {0, 1}),
        make_stmt(1726, {reg(13), 0, 0}),  // now wielded == 1
        make_stmt(1730, {0, 0, 1}),        // sets z to ground_level_z=2.0
        make_stmt(1731, {2, 0}),
        make_stmt(core_op::kPositionGetZ, {reg(14), 2}),
        make_stmt(1732, {0}),
        make_stmt(1735, {0}),
        make_stmt(1736, {0, 36}),
        make_stmt(1738, {reg(15), 0}),
        make_stmt(1740, {0, 12, 0}),
        make_stmt(1741, {0, 13}),
        make_stmt(1750, {0, 44}),
        make_stmt(1756, {reg(16), 0, 0}),
        make_stmt(1766, {0, 3}),
        make_stmt(1765, {reg(17), 0}),
        make_stmt(1770, {reg(18), 0}),
        make_stmt(1772, {reg(19), 0}),
        make_stmt(1783, {0, 2}),
        make_stmt(1773, {reg(20), 0}),
        make_stmt(1804, {reg(21), 0, 1}),

        // Entry points, orders, teams (1780, 1789-1796)
        make_stmt(1780, {3, 3, 0}),
        make_stmt(core_op::kPositionGetZ, {reg(22), 3}),
        make_stmt(1789, {}),
        make_stmt(1790, {0, -1, 5}),
        make_stmt(1791, {0, 2, 0}),
        make_stmt(1794, {4, 0, 2}),  // pos4 <- order pos div 2
        make_stmt(core_op::kPositionGetX, {reg(23), 4}),
        make_stmt(1792, {reg(24), 0}),
        make_stmt(1793, {0, 0}),
        make_stmt(1795, {0, 4}),
        make_stmt(1796, {0, 1, -1}),

        // Scene props & mission objects (1810-1820)
        make_stmt(1810, {reg(25), 15}),
        make_stmt(1811, {reg(26), 15, 1}),
        make_stmt(kNegFlag | 1811, {reg(26), 15, 5}),
        make_stmt(1813, {0, 0}),
        make_stmt(1812, {reg(27), 0}),
        make_stmt(1820, {0, 150}),
        make_stmt(1815, {reg(28), 0}),
        make_stmt(1818, {0, 2}),
        make_stmt(1817, {reg(29), 0}),

        // Mission / menu / trigger / timers (1906, 1907, 1911, 2060, 2075, 2370-2372)
        make_stmt(1906, {1}),
        make_stmt(1907, {4}),
        make_stmt(1911, {1}),
        make_stmt(2060, {9}),
        make_stmt(2075, {12345}),
        make_stmt(2370, {reg(30)}),  // (500000 - 500000)/100000 = 0
        make_stmt(2371, {reg(31)}),  // (250000 - 50000)/100000 = 2
        make_stmt(2372, {reg(32)}),  // (500000 - 0)/100000 = 5
    };

    StatementBlock replaceIgnored;
    replaceIgnored.statements = {make_stmt(1890, {15, 25})};
    StatementBlock replaceCallerM20;
    replaceCallerM20.statements = {make_stmt(1890, {15, 25})};
    StatementBlock replaceCallerM19;
    replaceCallerM19.statements = {make_stmt(1890, {25, 35})};

    const bool ok = execute_statement_block(block, 0, 0, {}, ctx);
    const bool okIgn = execute_statement_block(replaceIgnored, 0, 0, {}, ctx);
    const bool stayed15 = (w.mission_objects[0].scene_prop_no == 15);
    const bool okM20 = execute_statement_block(replaceCallerM20, -20, 0, {}, ctx);
    const bool became25 = (w.mission_objects[0].scene_prop_no == 25);
    const bool okM19 = execute_statement_block(replaceCallerM19, -19, 0, {}, ctx);
    const bool became35 = (w.mission_objects[1].scene_prop_no == 35);
    const bool callerPreserved = (ctx.currentCaller == 99);

    expect(ok && okIgn && stayed15 && okM20 && became25 && okM19 && became35 && callerPreserved &&
               ctx.errorCount == 0 && ctx.unimplementedCount == 0 &&
               ctx.registers[0] == 0 && ctx.registers[1] == 3 && ctx.registers[2] == 4 &&
               ctx.registers[3] == 900 && ctx.registers[4] == 1 && ctx.registers[5] == 0 &&
               ctx.registers[6] == 0 && ctx.registers[7] == 2 && ctx.registers[8] == 80 &&
               w.agents[0].hit_points == 50.0f && ctx.registers[9] == 5 &&
               ctx.registers[10] == 0 && ctx.registers[11] == 8 && ctx.registers[12] == 24 &&
               ctx.registers[13] == 1 && ctx.registers[14] == 200 && ctx.registers[15] == 4 &&
               w.agents[0].ai_alarmed && !w.agents[0].has_scripted_destination &&
               std::abs(w.agents[0].speed_limit - 10.0f) < 0.001f &&
               w.agents[0].animation == 12 && w.agents[0].stand_animation == 13 &&
               !w.played_sounds.empty() && ctx.registers[16] == 77 && ctx.registers[17] == 3 &&
               ctx.registers[18] == 0 && ctx.registers[19] == 1 && ctx.registers[20] == 2 &&
               ctx.registers[21] == 1 && ctx.registers[22] == 0 && !w.order_menu_open &&
               ctx.registers[23] == 900 && ctx.registers[24] == 1 && w.teams[0].leader_agent_no == 0 &&
               w.teams[0].order_listener == 4 && w.teams[0].relations[1] == -1 &&
               ctx.registers[25] == 2 && ctx.registers[26] == 1 && ctx.registers[27] == 0 &&
               ctx.registers[28] == 150 && w.mission_objects[0].max_hit_points == 150.0f &&
               ctx.registers[29] == 2 && w.mission_result == 1 && w.mission_finished &&
               w.mission_finish_delay == 4.0f && w.jump_mission_no == 1 &&
               w.jump_menu_no == 9 && w.trigger_result == 12345 &&
               ctx.registers[30] == 0 && ctx.registers[31] == 2 && ctx.registers[32] == 5,
           "all 66 agent/team/prop/mission/timer native opcodes match v1.1.5.6");
}

// Module scripts can free or reshape an object while the VM is iterating or
// sorting it. These cases crashed under AddressSanitizer before the loop frame
// and array_sort_custom stopped holding pointers into the live object.
void test_script_mutation_safety() {
    using namespace wse::vm;
    auto op = [](WseOpcode value) { return static_cast<std::uint32_t>(value); };

    {
        // dict_free inside try_for_dict_keys ends the loop after one pass.
        VmContext ctx;
        ctx.stringRegisters[1] = u"k1";
        ctx.stringRegisters[2] = u"k2";
        ctx.stringRegisters[3] = u"v";
        StatementBlock block;
        block.statements = {
            make_stmt(op(WseOpcode::dict_create), {reg(1)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(1), sreg(3)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(2), sreg(3)}),
            make_stmt(core_op::kTryForDictKeys, {sreg(10), reg(1)}),
            make_stmt(core_op::kValAdd, {reg(2), 1}),
            make_stmt(op(WseOpcode::dict_free), {reg(1)}),
            make_stmt(core_op::kTryEnd, {}),
        };
        expect(execute_statement_block(block, ctx) && ctx.errorCount == 0 &&
                   ctx.registers[2] == 1 && ctx.stringRegisters[10] == u"k1",
               "try_for_dict_keys stops cleanly when the body frees the dictionary");
    }

    {
        // Erasing the current key, and inserting keys, keeps the cursor valid:
        // k1 (erased, inserts k0 behind and k3 ahead), then k2, then k3.
        VmContext ctx;
        ctx.stringRegisters[0] = u"k0";
        ctx.stringRegisters[1] = u"k1";
        ctx.stringRegisters[2] = u"k2";
        ctx.stringRegisters[3] = u"k3";
        ctx.stringRegisters[4] = u"v";
        StatementBlock block;
        block.statements = {
            make_stmt(op(WseOpcode::dict_create), {reg(1)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(1), sreg(4)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(2), sreg(4)}),
            make_stmt(core_op::kTryForDictKeys, {sreg(10), reg(1)}),
            make_stmt(core_op::kValAdd, {reg(2), 1}),
            make_stmt(op(WseOpcode::dict_erase), {reg(1), sreg(10)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(0), sreg(4)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(3), sreg(4)}),
            make_stmt(core_op::kTryEnd, {}),
            make_stmt(op(WseOpcode::dict_get_size), {reg(3), reg(1)}),
        };
        expect(execute_statement_block(block, ctx) && ctx.errorCount == 0 &&
                   ctx.registers[2] == 3 && ctx.stringRegisters[10] == u"k3" &&
                   ctx.registers[3] == 2,
               "try_for_dict_keys survives erase of the current key and inserts");
    }

    {
        // dict_clear inside the loop ends it.
        VmContext ctx;
        ctx.stringRegisters[1] = u"k1";
        ctx.stringRegisters[2] = u"k2";
        ctx.stringRegisters[3] = u"v";
        StatementBlock block;
        block.statements = {
            make_stmt(op(WseOpcode::dict_create), {reg(1)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(1), sreg(3)}),
            make_stmt(op(WseOpcode::dict_set_str), {reg(1), sreg(2), sreg(3)}),
            make_stmt(core_op::kTryForDictKeys, {sreg(10), reg(1)}),
            make_stmt(core_op::kValAdd, {reg(2), 1}),
            make_stmt(op(WseOpcode::dict_clear), {reg(1)}),
            make_stmt(core_op::kTryEnd, {}),
        };
        expect(execute_statement_block(block, ctx) && ctx.errorCount == 0 &&
                   ctx.registers[2] == 1,
               "try_for_dict_keys stops cleanly when the body clears the dictionary");
    }

    {
        // Comparator frees the array being sorted.
        wse::game::ScriptManager scripts;
        scripts.scripts.resize(1);
        scripts.scripts[0].block.statements = {
            make_stmt(op(WseOpcode::array_free), {reg(5)}),
            make_stmt(op(WseOpcode::return_values), {1}),
        };
        VmContext ctx;
        ctx.scripts = &scripts;
        StatementBlock block;
        block.statements = {
            make_stmt(op(WseOpcode::array_create), {reg(5), 0, 4}),
            make_stmt(op(WseOpcode::array_set_val), {reg(5), 9, 0}),
            make_stmt(op(WseOpcode::array_set_val), {reg(5), 3, 1}),
            make_stmt(op(WseOpcode::array_sort_custom), {reg(5), 0}),
            make_stmt(core_op::kAssign, {reg(7), 1}),
        };
        expect(execute_statement_block(block, ctx) && ctx.errorCount == 0 &&
                   ctx.registers[7] == 1 &&
                   ctx.dictionaries.get_array(static_cast<std::int32_t>(ctx.registers[5])) ==
                       nullptr,
               "array_sort_custom survives a comparator that frees the array");
    }

    {
        // Comparator shrinks the array: the sorted copy no longer matches the
        // live shape, so the live array keeps the comparator's edits.
        wse::game::ScriptManager scripts;
        scripts.scripts.resize(1);
        scripts.scripts[0].block.statements = {
            make_stmt(core_op::kTryBegin, {}),
            make_stmt(core_op::kEq, {reg(8), 0}),
            make_stmt(core_op::kAssign, {reg(8), 1}),
            make_stmt(op(WseOpcode::array_pop), {reg(6), reg(5)}),
            make_stmt(core_op::kTryEnd, {}),
            make_stmt(core_op::kStoreScriptParam1, {loc(0)}),
            make_stmt(core_op::kStoreScriptParam2, {loc(1)}),
            make_stmt(core_op::kTryBegin, {}),
            make_stmt(core_op::kGe, {loc(1), loc(0)}),
            make_stmt(op(WseOpcode::return_values), {1}),
            make_stmt(core_op::kElseTry, {}),
            make_stmt(op(WseOpcode::return_values), {0}),
            make_stmt(core_op::kTryEnd, {}),
        };
        VmContext ctx;
        ctx.scripts = &scripts;
        StatementBlock block;
        block.statements = {
            make_stmt(op(WseOpcode::array_create), {reg(5), 0, 3}),
            make_stmt(op(WseOpcode::array_set_val), {reg(5), 9, 0}),
            make_stmt(op(WseOpcode::array_set_val), {reg(5), 3, 1}),
            make_stmt(op(WseOpcode::array_set_val), {reg(5), 5, 2}),
            make_stmt(op(WseOpcode::array_sort_custom), {reg(5), 0}),
            make_stmt(op(WseOpcode::array_get_dim_size), {reg(9), reg(5), 0}),
            make_stmt(op(WseOpcode::array_get_val), {reg(10), reg(5), 0}),
            make_stmt(op(WseOpcode::array_get_val), {reg(11), reg(5), 1}),
        };
        expect(execute_statement_block(block, ctx) && ctx.errorCount == 0 &&
                   ctx.registers[6] == 5 && ctx.registers[9] == 2 &&
                   ctx.registers[10] == 9 && ctx.registers[11] == 3,
               "array_sort_custom does not install a result over a reshaped array");
    }

    {
        // Modifier bits on a control opcode are masked the same way analyze()
        // masks them, so the try body still runs.
        VmContext ctx;
        StatementBlock block;
        block.statements = {
            make_stmt(core_op::kTryBegin | kNegFlag, {}),
            make_stmt(core_op::kAssign, {reg(1), 5}),
            make_stmt(core_op::kTryEnd | kNegFlag, {}),
            make_stmt(core_op::kAssign, {reg(2), 6}),
        };
        expect(execute_statement_block(block, ctx) && ctx.unimplementedCount == 0 &&
                   ctx.registers[1] == 5 && ctx.registers[2] == 6,
               "control opcodes carrying modifier bits execute as their base opcode");
    }

    {
        // convert_to_fixed_point wraps; a zeroed multiplier cannot divide.
        VmContext ctx;
        ctx.registers[1] = std::numeric_limits<std::int64_t>::min() + 1;
        ctx.registers[2] = 500;
        StatementBlock block;
        block.statements = {
            make_stmt(core_op::kConvertToFixedPoint, {reg(1)}),
            make_stmt(core_op::kConvertFromFixedPoint, {reg(2)}),
        };
        expect(execute_statement_block(block, ctx) && ctx.registers[1] == 100 &&
                   ctx.registers[2] == 5,
               "fixed-point conversions wrap on overflow and divide normally");
        ctx.fixedPointMultiplier = 0;
        StatementBlock zero;
        zero.statements = {make_stmt(core_op::kConvertFromFixedPoint, {reg(2)})};
        expect(execute_statement_block(zero, ctx) && ctx.errorCount == 1,
               "convert_from_fixed_point reports a zero multiplier instead of trapping");
    }
}

void test_locals_zeroing_default() {
    using namespace wse::vm;

    VmContext ctx;
    StatementBlock block;
    block.statements = {
        make_stmt(core_op::kValAdd, {loc(0), 5}),
        make_stmt(core_op::kAssign, {reg(0), loc(0)}),
        make_stmt(core_op::kAssign, {reg(1), loc(17)}),
    };
    expect(execute_statement_block(block, ctx) && ctx.registers[0] == 5 &&
               ctx.registers[1] == 0,
           "block execution initializes locals to zero by default");
}

void test_native_campaign_safety_deviations() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    auto& world = ctx.world;
    world.has_game = true;
    world.troops.resize(4);
    world.troops[0].flags = 0x10ULL;  // hero
    world.troops[1].upgrade_troop_nos = {2, 3};
    world.parties.resize(2);
    world.parties[0].active = true;
    world.parties[1].active = true;
    world.main_party_no = 0;
    world.player_troop_no = 0;
    world.sites.resize(1);
    world.current_site_no = 0;

    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();

    // add_xp_to_troop saturates instead of wrapping past INT32_MAX.
    world.troops[0].xp = kMax - 10;
    StatementBlock xpBlock;
    xpBlock.statements = {make_stmt(1062U, {20000, 0})};
    expect(execute_statement_block(xpBlock, ctx) && world.troops[0].xp == kMax,
           "add_xp_to_troop saturates troop xp at INT32_MAX");

    // party_add_prisoners saturates the stack size and reports the real delta.
    StatementBlock addBlock;
    addBlock.statements = {
        make_stmt(1611U, {1, 1, kMax}),
        make_stmt(core_op::kAssign, {reg(5), reg(0)}),
        make_stmt(1611U, {1, 1, kMax}),
    };
    expect(execute_statement_block(addBlock, ctx) && world.parties[1].stacks.size() == 1U &&
               world.parties[1].stacks[0].num_troops == kMax && ctx.registers[5] == kMax &&
               ctx.registers[0] == 0,
           "party_add_prisoners saturates num_troops and returns the applied delta");

    // party_force_add_members shares the saturating path in WseParty::add_members.
    StatementBlock forceBlock;
    forceBlock.statements = {
        make_stmt(1613U, {1, 2, kMax}),
        make_stmt(1613U, {1, 2, kMax}),
    };
    expect(execute_statement_block(forceBlock, ctx) && world.parties[1].stacks.size() == 2U &&
               world.parties[1].stacks[1].num_troops == kMax,
           "party_force_add_members saturates num_troops");

    // Invalid troop ids never reach a party stack.
    world.parties[1].stacks.clear();
    StatementBlock badTroopBlock;
    badTroopBlock.statements = {
        make_stmt(1611U, {1, -7, 5}),   // party_add_prisoners
        make_stmt(1612U, {1, 999, 1}),  // party_add_leader
    };
    expect(execute_statement_block(badTroopBlock, ctx) && world.parties[1].stacks.empty() &&
               ctx.registers[0] == 0,
           "party_add_prisoners and party_add_leader reject invalid troop ids");

    StatementBlock leaderBlock;
    leaderBlock.statements = {
        make_stmt(1611U, {1, 1, 3}),
        make_stmt(1612U, {1, 2, 1}),
    };
    expect(execute_statement_block(leaderBlock, ctx) && world.parties[1].stacks.size() == 2U &&
               world.parties[1].stacks[0].troop_no == 2 &&
               !world.parties[1].stacks[0].is_prisoner(),
           "party_add_leader still inserts a valid leader at stack 0");

    // add_visitors_to_current_scene is capped at kMaxSiteVisitors.
    StatementBlock visitorBlock;
    visitorBlock.statements = {make_stmt(1265U, {0, 1, kMax})};
    expect(execute_statement_block(visitorBlock, ctx) &&
               world.sites[0].visitors.size() == wse::game::kMaxSiteVisitors,
           "add_visitors_to_current_scene caps the visitor list");

    // Squared-distance and gold-as-xp conversions saturate instead of overflowing.
    ctx.positions[1] = wse::core::WseTransform::identity();
    ctx.positions[2] = wse::core::WseTransform::identity();
    ctx.positions[1].o.x = 3.0e18F;
    world.troops[0].xp = 0;
    StatementBlock castBlock;
    castBlock.statements = {
        make_stmt(712U, {reg(6), 1, 2}),
        make_stmt(713U, {reg(7), 1, 2}),
        make_stmt(1063U, {2000000000, 0}),
    };
    expect(execute_statement_block(castBlock, ctx) &&
               ctx.registers[6] == std::numeric_limits<std::int64_t>::max() &&
               ctx.registers[7] == std::numeric_limits<std::int64_t>::max() &&
               world.troops[0].xp == 29999,
           "sq-distance and add_gold_as_xp saturate out-of-range float results");

    // WSE party_stack_upgrade treats any nonzero path as path 1.
    world.parties[1].stacks.clear();
    world.parties[1].stacks.push_back({1, 4, 0, 0});
    world.parties[1].stacks[0].num_upgradeable = 4;
    StatementBlock upgradeBlock;
    upgradeBlock.statements = {make_stmt(3905U, {1, 0, 4, 2})};
    expect(execute_statement_block(upgradeBlock, ctx) && world.parties[1].stacks.size() == 1U &&
               world.parties[1].stacks[0].troop_no == 3 &&
               world.parties[1].stacks[0].num_troops == 4,
           "party_stack_upgrade with path 2 upgrades along the second path");
}

// mbParty::getMaxNumCompanions (0x1401c18a0) / getMaxNumPrisoners (0x1401c1920) run the
// module's limit scripts first; getTroopSkill (0x1400dcd50) adds the skill-modifier script.
void test_party_limits_and_skill_modifier() {
    using namespace wse::vm;
    VmContext ctx;
    auto& world = ctx.world;
    world.troops.resize(1);
    world.troops[0].flags = 0x10ULL;
    world.troops[0].attributes = {10, 10, 10, 7};  // charisma 7
    world.troops[0].set_skill_raw(1, 4);           // leadership
    world.troops[0].set_skill_raw(2, 3);           // prisoner management
    world.skills[1].max_level = 10;
    world.skills[2].max_level = 10;
    world.parties.resize(1);
    world.parties[0].stacks.push_back({0, 1, 0, 0});
    world.main_party_no = 0;

    const auto run = [&]() {
        StatementBlock block;
        block.statements = {
            make_stmt(1633, {reg(40), 0}),  // party_get_free_companions_capacity
            make_stmt(1634, {reg(41), 0}),  // party_get_free_prisoners_capacity
            make_stmt(2170, {reg(42), 1, 0}),  // store_skill_level leadership
        };
        return execute_statement_block(block, ctx);
    };

    expect(run() && ctx.registers[40] == 20 + 4 * 3 + 7 - 1 && ctx.registers[41] == 3 * 5 &&
               ctx.registers[42] == 4,
           "without module scripts: 20 + leadership*3 + charisma, prisoner management*5");

    wse::game::ScriptManager scripts;
    scripts.scripts.resize(3);
    scripts.scripts[0].block.statements = {make_stmt(2075, {2})};    // skill modifier +2
    scripts.scripts[1].block.statements = {make_stmt(2075, {100})};  // companion limit
    scripts.scripts[2].block.statements = {make_stmt(2075, {9})};    // prisoner limit
    scripts.mappedScripts[0x1b] = 0;
    ctx.scripts = &scripts;
    expect(run() && ctx.registers[42] == 6 && ctx.registers[40] == 20 + 6 * 3 + 7 - 1 &&
               ctx.registers[41] == 5 * 5,
           "game_get_skill_modifier_for_troop feeds store_skill_level and the built-in limits");

    scripts.scripts[0].block.statements = {make_stmt(2075, {20})};
    expect(troop_skill_level(ctx, 0, 1) == 10, "a skill modifier is clamped to the skill's maximum");

    scripts.mappedScripts[0x15] = 1;
    scripts.mappedScripts[0x16] = 2;
    expect(run() && ctx.registers[40] == 99 && ctx.registers[41] == 9,
           "game_get_party_companion_limit / game_get_party_prisoner_limit override the formula");
    StatementBlock join;
    join.statements = {make_stmt(105, {99})};  // troops_can_join 99 with 1 companion, limit 100
    const bool can99 = execute_statement_block(join, ctx);
    join.statements = {make_stmt(105, {100})};
    expect(can99 && !execute_statement_block(join, ctx),
           "troops_can_join uses the module's companion limit");

    // A mapped script run from inside a depth-0 block gets its own frame: the modifier script
    // below loops over local 0 and writes local 1, which the outer block also uses.
    scripts.scripts[0].block = StatementBlock{};
    scripts.scripts[0].block.statements = {
        make_stmt(6, {loc(0), 0, 3}),  // try_for_range
        make_stmt(2133, {loc(1), 5}),  // assign
        make_stmt(3, {}),              // try_end
        make_stmt(2075, {1}),          // set_trigger_result
    };
    StatementBlock outer;
    outer.statements = {
        make_stmt(2133, {loc(1), 77}),
        make_stmt(2133, {reg(44), 0}),
        make_stmt(6, {loc(0), 0, 5}),
        make_stmt(2170, {reg(43), 1, 0}),
        make_stmt(2105, {reg(44), 1}),  // val_add
        make_stmt(3, {}),
        make_stmt(2133, {reg(45), loc(1)}),
    };
    expect(execute_statement_block(outer, ctx) && ctx.registers[44] == 5 &&
               ctx.registers[45] == 77 && ctx.registers[43] == 5 && ctx.activeBlocks == 0,
           "a mapped script called mid-block keeps the caller's locals and loop counter");
}

void test_native_campaign_ops() {
    using namespace wse::vm;

    VmContext ctx;
    ctx.failOnUnimplemented = true;
    auto& world = ctx.world;

    // Troops: 0=player(hero), 1=peasant, 2=footman(upgraded from 1)
    world.troops.resize(3);
    world.troops[0].flags = 0x10ULL;  // hero
    world.troops[0].level = 6;
    world.troops[0].attributes = {12, 10, 10, 10};
    world.troops[1].name = "Peasant";
    world.troops[1].upgrade_troop_nos = {2, -1};
    world.troops[2].name = "Footman";
    world.troops[2].flags = (1ULL << 26U);  // guarantee_ranged

    // Items: 0=horse, 1=sword, 2=helmet
    world.items.resize(3);
    world.items[0].properties = 1ULL;  // itp_type_horse
    world.items[0].horse_speed = 45;
    world.items[0].value = 500;
    world.items[1].properties = 2ULL;  // itp_type_one_handed_wpn
    world.items[1].weight = 2.5f;
    world.items[1].thrust_damage = 28;
    world.items[1].swing_damage = 34;
    world.items[2].properties = 12ULL;  // itp_type_head_armor
    world.items[2].head_armor = 40;

    // Factions
    world.factions.resize(2);
    world.factions[0].set_relation(1, -0.5f);

    // Party templates & parties: 0=main_party, 1=town
    world.party_templates.resize(1);
    world.party_templates[0].name = "Looters";
    world.party_templates[0].stacks[0] = {1, 3, 5, 0};

    world.parties.resize(2);
    world.parties[0].name = "Player";
    world.parties[0].radius = 1.0f;
    world.parties[0].behavior = 4;
    world.parties[0].behavior_object = 1;
    world.parties[0].stacks.push_back({0, 1, 0, 0});
    world.parties[1].name = "Suno";
    world.parties[1].position.o = {10.0f, 0.0f, 0.0f, 1.0f};
    world.encountered_party_no = 1;

    // Scene props / mission objects & agents
    world.mission_objects.resize(1);
    world.mission_objects[0].scene_prop_no = 5;
    world.agents.resize(2);
    world.agents[0].troop_no = 0;
    world.agents[0].team_no = 0;
    world.agents[0].hit_points = 100.0f;
    world.agents[0].max_hit_points = 100.0f;
    world.agents[1].troop_no = 1;
    world.agents[1].team_no = 1;
    world.agents[1].hit_points = 50.0f;
    world.agents[1].max_hit_points = 50.0f;
    world.player_agent_no = 0;
    world.teams[0].order_listener = 1;
    world.teams[0].order_types[0] = 1;
    world.teams[0].relations[1] = -1;
    world.teams[1].relations[0] = -1;

    // Local strings & positions
    const std::array<std::u16string_view, 2> kLocalStrs{
        u"Hero Face Result",
        u"0000000100020003000400050006000700080009000a000b000c000d000e000f",
    };
    ctx.localStrings = kLocalStrs;
    ctx.positions[1].o = {3.0f, 4.0f, 5.0f, 1.0f};
    ctx.positions[2].o = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx.timersTotal = {250000, 500000, 0};  // 2.5s on timer A
    world.mission_timers[0] = {0, 100000};  // 1.5s elapsed
    world.ground_level_z = 1.0f;

    StatementBlock b;
    b.statements = {
        make_stmt(36U, {1}),                              // entering_town(1) -> true
        make_stmt(37U, {}),                               // map_free -> true
        make_stmt(60U, {static_cast<std::int64_t>(tagged(0x16, 0))}),
        make_stmt(153U, {2}),                             // troop_is_guarantee_ranged(2) -> true
        make_stmt(371U, {2, 3, 42}),                      // set_achievement_stat
        make_stmt(370U, {reg(17), 2, 3}),                 // get_achievement_stat -> 42
        make_stmt(713U, {reg(1), 1, 2}),                  // sq_dist_meters = 50
        make_stmt(792U, {reg(2), 1}),                     // (5 - 1) * 100 = 400
        make_stmt(1005U, {2}),                            // num_active_teams_le(2) -> true
        make_stmt(1062U, {250, 0}),                       // add_xp_to_troop
        make_stmt(1515U, {reg(3), 0}),                    // troop_get_xp == 250
        make_stmt(1103U, {4}),                            // set_spawn_radius
        make_stmt(1100U, {1, 0}),                         // spawn_around_party -> reg0 = 2
        make_stmt(core_op::kAssign, {reg(4), reg(0)}),    // save spawned party_no = 2
        make_stmt(1543U, {0, 8, 0}),                      // give horse to slot 8
        make_stmt(152U, {0}),                             // troop_is_mounted(0) -> true
        make_stmt(151U, {0, 0}),                          // troop_has_item_equipped(0, 0) -> true
        make_stmt(1561U, {reg(5), 1, 0}),                 // troop_get_upgrade_troop -> 2
        make_stmt(1611U, {0, 1, 4}),                      // party_add_prisoners(main, 1, 4)
        make_stmt(1656U, {reg(6), 0, 0}),                 // prisoner_stack troop == 1
        make_stmt(1672U, {0, 75}),                        // party_set_morale
        make_stmt(1671U, {reg(7), 0}),                    // party_get_morale == 75
        make_stmt(1775U, {0, 1}),                         // class_is_listening_order -> true
        make_stmt(1785U, {reg(8), 0, 0}),                 // team_get_movement_order -> 0
        make_stmt(1788U, {0, 1}),                         // teams_are_enemies -> true
        make_stmt(1779U, {0, 1, 1}),                      // agent_equip_item
        make_stmt(1722U, {0, 1, 60}),                     // kill enemy agent 1 (50hp - 60)
        make_stmt(1003U, {}),                             // all_enemies_defeated -> true!
        make_stmt(1854U, {0, 200, 150, 100}),             // prop_instance_set_scale
        make_stmt(1860U, {0, 1, 50}),                     // prop animate_to_position
        make_stmt(1862U, {reg(9), 0}),                    // is_animating == 1
        make_stmt(2091U, {0, 150}),                       // agent_set_damage_modifier
        make_stmt(2065U, {reg(10), 0}),                   // agent_get_damage_modifier == 150
        make_stmt(2154U, {reg(11), 0}),                   // store_num_free_stacks == 10
        make_stmt(2190U, {reg(12), 0, 1}),                // store_relation == -50
        make_stmt(2365U, {reg(13)}),                      // mission_timer_a_msec == 1500
        make_stmt(2375U, {}),                             // reset_mission_timer_a
        make_stmt(2365U, {reg(14)}),                      // mission_timer_a_msec == 0
        make_stmt(2700U, {reg(15), 1}),                   // item_get_weight(1) == 250
        make_stmt(2718U, {reg(16), 1}),                   // item_get_thrust_damage(1) == 28
        make_stmt(2723U, {1, 2}),                         // item_has_property(1, 2) -> true
        make_stmt(2751U, {0, static_cast<std::int64_t>(tagged(0x16, 1)), 0}),
        make_stmt(2750U, {0, 0, 0}),                      // store into s0
    };

    expect(execute_statement_block(b, ctx) && ctx.errorCount == 0 &&
               ctx.unimplementedCount == 0,
           "M3 campaign opcodes execute without errors or unimplemented hits");
    expect(world.result_string == "Hero Face Result" &&
               ctx.registers[0] == 4 && ctx.registers[17] == 42 &&
               ctx.registers[1] == 50 && ctx.registers[2] == 400 &&
               ctx.registers[3] == 250 && ctx.registers[4] == 2 && ctx.registers[5] == 2 &&
               ctx.registers[6] == 1 && ctx.registers[7] == 75 && ctx.registers[8] == 0 &&
               ctx.registers[9] == 1 && ctx.registers[10] == 150 && ctx.registers[11] == 10 &&
               ctx.registers[12] == -50 && ctx.registers[13] == 1500 &&
               ctx.registers[14] == 0 && ctx.registers[15] == 250 &&
               ctx.registers[16] == 28 && !world.agents[1].is_alive() &&
               world.agents[0].equipped_items[0] == 1 &&
               ctx.stringRegisters[0] ==
                   u"0000000100020003000400050006000700080009000a000b000c000d000e000f",
           "M3 campaign opcodes produce exact verified state and outputs");
}

}  // namespace

namespace wse::selftest {

int run_block_vm_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_analyze_block();
    test_modifiers_and_top_level_failure();
    test_try_else_and_loops();
    test_native_core_math_and_hook();
    test_scripts_recursion_and_depth_limit();
    test_triggers_and_wse_integration();
    test_return_values_positions_and_dict_loop();
    test_feval_and_custom_array_sort();
    test_world_state_script_integration();
    test_unimplemented_histogram_and_loud_failure();
    test_native_entity_slots();
    test_native_arithmetic_registers_and_vars();
    test_native_positions_and_math();
    test_native_strings_and_entity_ops();
    test_entity_iterators();
    test_native_entity_ops();
    test_native_mission_ops();
    test_script_mutation_safety();
    test_locals_zeroing_default();
    test_native_campaign_ops();
    test_party_limits_and_skill_modifier();
    test_native_campaign_safety_deviations();

    if (failures != 0) {
        std::printf("block_vm tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("block_vm tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_block_vm_suite(nullptr);
}
#endif
