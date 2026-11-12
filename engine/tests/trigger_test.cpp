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

}  // namespace

int main() {
    test_trigger();
    test_trigger_id();
    test_manager();
    test_manager_errors();
    test_simple_trigger();

    if (failures != 0) {
        std::printf("trigger tests: FAIL (%d)\n", failures);
        return 1;
    }
    std::printf("trigger tests: PASS\n");
    return 0;
}
