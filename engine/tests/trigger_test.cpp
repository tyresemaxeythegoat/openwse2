// Host tests for the reconstructed trigger readers.
//
// Build/run: powershell -File engine/build_host.ps1

#include <cstdint>
#include <cstdio>
#include <string_view>

#include "wse/game/trigger.h"
#include "wse/io/text_stream.h"
#include "wse/vm/statement.h"

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

void test_trigger() {
    using namespace wse::game;
    using namespace wse;
    using namespace wse::vm;

    // Three intervals, one condition statement, no consequence statements.
    io::TokenReader reader(
        "1.5 0.25 3.0\n"
        "1\n"
        "1 1 7\n"
        "0\n");

    Trigger trigger;
    expect(read_trigger(reader, trigger, "Game", 0) == ReadError::none, "trigger reads");
    expect(trigger.checkInterval == 1.5F, "check interval");
    expect(trigger.delayInterval == 0.25F, "delay interval");
    expect(trigger.rearmInterval == 3.0F, "rearm interval");
    expect(trigger.status == 0, "status defaults to zero");
    expect(trigger.conditions.statements.size() == 1, "conditions statement count");
    expect(trigger.conditions.statements[0].opcode == 1, "conditions opcode");
    expect(trigger.conditions.statements[0].operands[0] == 7, "conditions operand");
    expect(trigger.consequences.statements.empty(), "consequences are empty");
    expect(trigger.conditions.id == "Game Trigger [0] Conditions", "conditions id");
    expect(trigger.consequences.id == "Game Trigger [0] Consequences", "consequences id");
    expect(reader.at_end(), "trigger consumes its input");
}

void test_trigger_id() {
    using namespace wse::game;
    expect(trigger_id("Game", 12, " Conditions") == "Game Trigger [12] Conditions",
           "id formatting with suffix");
    expect(trigger_id("Simple", 3, "") == "Simple Trigger [3]", "id formatting without suffix");
}

void test_manager() {
    using namespace wse::game;
    using namespace wse::vm;
    using namespace wse;

    io::TokenReader reader(
        "2\n"
        "1 0 0\n"
        "1\n"
        "1 1 5\n"
        "0\n"
        "2 0 0\n"
        "0\n"
        "0\n");

    const std::int64_t timersTotal[3] = {100, 200, 300};
    TriggerManager manager;
    manager.defaultTimerNo = 1;
    expect(read_trigger_manager(reader, manager, "Game", timersTotal) == ReadError::none,
           "trigger manager reads");
    expect(manager.triggers.size() == 2, "manager trigger count");

    const Trigger& first = manager.triggers[0];
    expect(first.checkTimer.timerNo == 1 && first.checkTimer.value == 200,
           "check timer seeded from g_timersTotal");
    expect(first.delayTimer.value == 200 && first.rearmTimer.value == 200,
           "all timers share the default timer value");
    expect(first.conditions.statements.size() == 1, "first trigger conditions");
    expect(manager.triggers[1].conditions.statements.empty(), "second trigger conditions");
    expect(manager.triggers[1].consequences.statements.empty(), "second trigger consequences");
    expect(reader.at_end(), "manager consumes its input");
}

void test_manager_errors() {
    using namespace wse::game;
    using namespace wse::vm;
    using namespace wse;

    const std::int64_t timersTotal[2] = {0, 0};

    {
        io::TokenReader reader("-1");
        TriggerManager manager;
        expect(read_trigger_manager(reader, manager, "Game", timersTotal) ==
                   ReadError::negative_count,
               "negative trigger count is rejected");
    }

    {
        io::TokenReader reader("65537");
        TriggerManager manager;
        expect(read_trigger_manager(reader, manager, "Game", timersTotal) ==
                   ReadError::trigger_count_out_of_range,
               "trigger count above the cap is rejected");
    }

    {
        io::TokenReader reader("1\n1 0 0\n0\n0\n");
        TriggerManager manager;
        manager.defaultTimerNo = 5;
        expect(read_trigger_manager(reader, manager, "Game", timersTotal) ==
                   ReadError::timer_index_out_of_range,
               "out-of-range default timer is rejected");
    }

    {
        io::TokenReader reader("1\n1 0 0\n1\n1 33\n");
        TriggerManager manager;
        expect(read_trigger_manager(reader, manager, "Game", timersTotal) ==
                   ReadError::operand_count_out_of_range,
               "statement error propagates out of the manager");
    }
}

void test_simple_trigger() {
    using namespace wse::game;
    using namespace wse::vm;
    using namespace wse;

    io::TokenReader reader(
        "1\n"
        "2.5\n"
        "1\n"
        "1 1 9\n");

    const std::int64_t timersTotal[1] = {7};
    SimpleTriggerManager manager;
    expect(read_simple_trigger_manager(reader, manager, "Simple", timersTotal) ==
               ReadError::none,
           "simple trigger manager reads");
    expect(manager.triggers.size() == 1, "simple trigger count");
    expect(manager.triggers[0].interval == 2.5F, "simple trigger interval");
    expect(manager.triggers[0].timer.timerNo == 0 && manager.triggers[0].timer.value == 7,
           "simple trigger timer seeded");
    expect(manager.triggers[0].block.statements.size() == 1, "simple trigger statement count");
    expect(manager.triggers[0].block.statements[0].operands[0] == 9,
           "simple trigger statement operand");
    expect(manager.triggers[0].block.id == "Simple Trigger [0]", "simple trigger id");
}

struct ActionLog {
    int checks = 0;
    int consequences = 0;
    int blocks = 0;
    bool conditionsResult = true;
};

bool on_conditions(void* context, const wse::vm::StatementBlock&) {
    auto* log = static_cast<ActionLog*>(context);
    ++log->checks;
    return log->conditionsResult;
}

void on_consequences(void* context, const wse::vm::StatementBlock&) {
    ++static_cast<ActionLog*>(context)->consequences;
}

void on_block(void* context, const wse::vm::StatementBlock&) {
    ++static_cast<ActionLog*>(context)->blocks;
}

void test_interval_trigger() {
    using namespace wse::game;
    using namespace wse::vm;

    std::int64_t timersTotal[1] = {0};
    ActionLog log;
    const TriggerCallbacks callbacks{on_conditions, on_consequences, &log};

    Trigger trigger;
    trigger.checkInterval = 1.0F;
    trigger.delayInterval = 2.0F;
    trigger.rearmInterval = 3.0F;
    trigger.checkTimer = AppTimer{0, 0};
    trigger.delayTimer = AppTimer{0, 0};
    trigger.rearmTimer = AppTimer{0, 0};

    expect(execute_trigger(trigger, 0, timersTotal, callbacks), "idle tick succeeds");
    expect(log.checks == 0 && trigger.status == 0, "check interval not yet elapsed");

    timersTotal[0] = 100000;  // 1 second
    expect(execute_trigger(trigger, 0, timersTotal, callbacks), "check tick succeeds");
    expect(log.checks == 1 && trigger.status == 1, "conditions pass and delay starts");
    expect(trigger.checkTimer.value == 100000 && trigger.delayTimer.value == 100000,
           "check and delay timers latch the current tick");

    expect(execute_trigger(trigger, 0, timersTotal, callbacks), "delay tick succeeds");
    expect(log.consequences == 0 && trigger.status == 1, "delay has not elapsed");

    timersTotal[0] = 300000;  // 2 more seconds
    expect(execute_trigger(trigger, 0, timersTotal, callbacks), "delay expiry succeeds");
    expect(log.consequences == 1 && trigger.status == 2, "consequences run after the delay");
    expect(trigger.rearmTimer.value == 300000, "rearm timer latches at consequence time");

    timersTotal[0] = 600000;  // 3 more seconds
    expect(execute_trigger(trigger, 0, timersTotal, callbacks), "rearm expiry succeeds");
    expect(trigger.status == 0, "rearm returns the trigger to idle");
    expect(trigger.checkTimer.value == 600000, "rearm resets the check timer");
    expect(log.checks == 1, "rearm alone does not evaluate conditions");

    timersTotal[0] = 700000;
    expect(execute_trigger(trigger, 0, timersTotal, callbacks), "next check succeeds");
    expect(log.checks == 2 && trigger.status == 1, "trigger re-arms and fires again");
}

void test_event_trigger() {
    using namespace wse::game;

    std::int64_t timersTotal[1] = {500000};
    ActionLog log;
    const TriggerCallbacks callbacks{on_conditions, on_consequences, &log};

    Trigger trigger;
    trigger.checkInterval = -3.0F;
    trigger.delayInterval = 10.0F;
    trigger.rearmInterval = 10.0F;
    trigger.rearmTimer = AppTimer{0, 0};

    expect(execute_trigger(trigger, -2, timersTotal, callbacks), "wrong-key tick succeeds");
    expect(log.checks == 0 && log.consequences == 0 && trigger.status == 0,
           "negative interval fires only on its rounded event key");

    expect(execute_trigger(trigger, -3, timersTotal, callbacks), "matching-key tick succeeds");
    expect(log.checks == 1 && log.consequences == 1 && trigger.status == 2,
           "event trigger evaluates conditions and runs consequences immediately");
    expect(trigger.rearmTimer.value == 500000, "event trigger latches the rearm timer");

    log.conditionsResult = false;
    trigger.status = 0;
    trigger.checkInterval = -4.0F;
    expect(execute_trigger(trigger, -4, timersTotal, callbacks), "failed-key tick succeeds");
    expect(log.checks == 2 && log.consequences == 1 && trigger.status == 0,
           "failed event conditions leave the trigger idle");
}

void test_simple_trigger_execute() {
    using namespace wse::game;

    std::int64_t timersTotal[1] = {0};
    ActionLog log;
    const SimpleTriggerCallbacks callbacks{on_block, &log};

    SimpleTrigger trigger;
    trigger.interval = 1.0F;
    trigger.timer = AppTimer{0, 0};

    expect(execute_simple_trigger(trigger, 0, timersTotal, callbacks), "simple idle succeeds");
    expect(log.blocks == 0, "simple interval not yet elapsed");

    timersTotal[0] = 100000;
    expect(execute_simple_trigger(trigger, 0, timersTotal, callbacks), "simple expiry succeeds");
    expect(log.blocks == 1 && trigger.timer.value == 100000, "simple block runs and latches");

    timersTotal[0] = 150000;
    expect(execute_simple_trigger(trigger, 0, timersTotal, callbacks), "simple early tick");
    expect(log.blocks == 1, "simple interval suppresses early execution");

    timersTotal[0] = 200000;
    expect(execute_simple_trigger(trigger, 0, timersTotal, callbacks), "simple re-fire");
    expect(log.blocks == 2, "simple block runs again at the next interval");

    trigger.interval = -2.0F;
    expect(execute_simple_trigger(trigger, 1, timersTotal, callbacks), "simple wrong key");
    expect(log.blocks == 2, "simple event trigger ignores other keys");
    expect(execute_simple_trigger(trigger, -2, timersTotal, callbacks), "simple matching key");
    expect(log.blocks == 3, "simple event trigger runs its block");
}

void test_trigger_timer_bounds() {
    using namespace wse::game;

    std::int64_t timersTotal[1] = {0};
    ActionLog log;
    const TriggerCallbacks callbacks{on_conditions, on_consequences, &log};
    const SimpleTriggerCallbacks simpleCallbacks{on_block, &log};

    Trigger trigger;
    trigger.checkTimer = AppTimer{9, 0};
    expect(!execute_trigger(trigger, 0, timersTotal, callbacks),
           "out-of-range check timer is rejected");
    expect(log.checks == 0, "rejected trigger does not evaluate conditions");

    SimpleTrigger simple;
    simple.timer = AppTimer{9, 0};
    expect(!execute_simple_trigger(simple, 0, timersTotal, simpleCallbacks),
           "out-of-range simple timer is rejected");
    expect(log.blocks == 0, "rejected simple trigger does not run its block");
}

void test_interval_key() {
    using namespace wse::game;
    expect(interval_key(2.4F) == 2, "2.4 rounds to 2");
    expect(interval_key(2.5F) == 2, "2.5 rounds down to 2");
    expect(interval_key(2.6F) == 3, "2.6 rounds to 3");
    expect(interval_key(-3.4F) == -3, "-3.4 rounds to -3");
    expect(interval_key(-3.5F) == -4, "-3.5 rounds down to -4");
    expect(interval_key(-3.6F) == -4, "-3.6 rounds to -4");
}

}  // namespace

namespace wse::selftest {

int run_trigger_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_trigger();
    test_trigger_id();
    test_manager();
    test_manager_errors();
    test_simple_trigger();
    test_interval_trigger();
    test_event_trigger();
    test_simple_trigger_execute();
    test_trigger_timer_bounds();
    test_interval_key();

    if (failures != 0) {
        std::printf("trigger tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("trigger tests: PASS\n");
    return 0;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_trigger_suite(nullptr);
}
#endif
