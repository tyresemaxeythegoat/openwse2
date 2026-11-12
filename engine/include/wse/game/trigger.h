// Trigger records and their module-text readers.
//
// Evidence (Ghidra 12.1.4 pseudocode):
//   mbTriggerManager::readStream         0x14023ea20
//   mbTrigger::readStream                0x14023e750
//   mbSimpleTriggerManager::readStream   0x14020b9d0
//   mbSimpleTrigger::readStream          0x14020b620
//   mbGame::readTriggers                 0x1400cca30
//   mbGame::readSimpleTriggers           0x1400ccde0
//
// Layouts (PDB):
//   mbTrigger             256 bytes type 0x077c9
//     +0x000 check/delay/rearm intervals (3 floats), +0x00c status
//     +0x010 conditions block, +0x070 consequences block
//     +0x0d0/+0x0e0/+0x0f0 check/delay/rearm rglAppTimer
//   mbSimpleTrigger       120 bytes type 0x071a6
//     +0x000 interval, +0x008 statement block, +0x068 timer
//   rglTimer               16 bytes type 0x164d9
//     +0x000 m_timerNo int32, +0x008 m_value int64
//
// Both managers read a count and then construct that many entries, seeding
// every timer with m_defaultTimerNo and g_timersTotal[m_defaultTimerNo]
// (0x1405d0ef0, public symbol ?g_timersTotal@@3PA_KA).
//
// mbGame::readTriggers opens "triggers.txt", discards a three-token header
// ("triggersfile version 1" in the retail files), then calls the manager
// reader with the prefix "Game". readSimpleTriggers does the same with
// "simple_triggers.txt", "Simple", and mbSimpleTriggerManager. The prefixes
// become the block ids "Game Trigger [i] Conditions" and
// "Simple Trigger [i]".

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/io/text_stream.h"
#include "wse/vm/statement.h"

namespace wse::game {

inline constexpr std::size_t kMaxTriggers = 1u << 16;

struct AppTimer {
    std::int32_t timerNo = 0;
    std::int64_t value = 0;
};

struct Trigger {
    float checkInterval = 0.0F;
    float delayInterval = 0.0F;
    float rearmInterval = 0.0F;
    std::int32_t status = 0;
    vm::StatementBlock conditions;
    vm::StatementBlock consequences;
    AppTimer checkTimer;
    AppTimer delayTimer;
    AppTimer rearmTimer;
};

struct SimpleTrigger {
    float interval = 0.0F;
    vm::StatementBlock block;
    AppTimer timer;
};

struct TriggerManager {
    std::vector<Trigger> triggers;
    std::int32_t defaultTimerNo = 0;
};

struct SimpleTriggerManager {
    std::vector<SimpleTrigger> triggers;
    std::int32_t defaultTimerNo = 0;
};

// rglFormat("%s Trigger [%d]%s", prefix, index, suffix).
[[nodiscard]] inline std::string trigger_id(std::string_view prefix,
                                            std::int32_t index,
                                            std::string_view suffix) {
    std::string id;
    id.reserve(prefix.size() + suffix.size() + 24);
    id.append(prefix);
    id.append(" Trigger [");
    id.append(std::to_string(index));
    id.push_back(']');
    id.append(suffix);
    return id;
}

[[nodiscard]] inline vm::ReadError seed_timer(AppTimer& timer,
                                              std::int32_t timerNo,
                                              std::span<const std::int64_t> timersTotal) {
    if (timerNo < 0 || static_cast<std::size_t>(timerNo) >= timersTotal.size()) {
        return vm::ReadError::timer_index_out_of_range;
    }
    timer.timerNo = timerNo;
    timer.value = timersTotal[static_cast<std::size_t>(timerNo)];
    return vm::ReadError::none;
}

// Three floats, the conditions block, then the consequences block.
[[nodiscard]] inline vm::ReadError read_trigger(io::TokenReader& reader,
                                                Trigger& trigger,
                                                std::string_view prefix,
                                                std::int32_t index) {
    if (!reader.read_f32(trigger.checkInterval) || !reader.read_f32(trigger.delayInterval) ||
        !reader.read_f32(trigger.rearmInterval)) {
        return vm::ReadError::malformed_number;
    }

    vm::ReadError error = vm::read_statement_block(reader, trigger.conditions);
    if (error != vm::ReadError::none) {
        return error;
    }
    error = vm::read_statement_block(reader, trigger.consequences);
    if (error != vm::ReadError::none) {
        return error;
    }

    trigger.conditions.id = trigger_id(prefix, index, " Conditions");
    trigger.consequences.id = trigger_id(prefix, index, " Consequences");
    return vm::ReadError::none;
}

// One float, then the statement block.
[[nodiscard]] inline vm::ReadError read_simple_trigger(io::TokenReader& reader,
                                                       SimpleTrigger& trigger,
                                                       std::string_view prefix,
                                                       std::int32_t index) {
    if (!reader.read_f32(trigger.interval)) {
        return vm::ReadError::malformed_number;
    }
    const vm::ReadError error = vm::read_statement_block(reader, trigger.block);
    if (error != vm::ReadError::none) {
        return error;
    }
    trigger.block.id = trigger_id(prefix, index, "");
    return vm::ReadError::none;
}

[[nodiscard]] inline vm::ReadError read_trigger_manager(
    io::TokenReader& reader,
    TriggerManager& manager,
    std::string_view prefix,
    std::span<const std::int64_t> timersTotal) {
    std::int32_t count = 0;
    if (!reader.read_i32(count)) {
        return vm::ReadError::malformed_number;
    }
    if (count < 0) {
        return vm::ReadError::negative_count;
    }
    if (static_cast<std::uint64_t>(count) > kMaxTriggers) {
        return vm::ReadError::trigger_count_out_of_range;
    }

    manager.triggers.assign(static_cast<std::size_t>(count), Trigger{});
    for (std::int32_t index = 0; index < count; ++index) {
        Trigger& trigger = manager.triggers[static_cast<std::size_t>(index)];
        vm::ReadError error =
            seed_timer(trigger.checkTimer, manager.defaultTimerNo, timersTotal);
        if (error == vm::ReadError::none) {
            error = seed_timer(trigger.delayTimer, manager.defaultTimerNo, timersTotal);
        }
        if (error == vm::ReadError::none) {
            error = seed_timer(trigger.rearmTimer, manager.defaultTimerNo, timersTotal);
        }
        if (error != vm::ReadError::none) {
            return error;
        }
        error = read_trigger(reader, trigger, prefix, index);
        if (error != vm::ReadError::none) {
            return error;
        }
    }
    return vm::ReadError::none;
}

[[nodiscard]] inline vm::ReadError read_simple_trigger_manager(
    io::TokenReader& reader,
    SimpleTriggerManager& manager,
    std::string_view prefix,
    std::span<const std::int64_t> timersTotal) {
    std::int32_t count = 0;
    if (!reader.read_i32(count)) {
        return vm::ReadError::malformed_number;
    }
    if (count < 0) {
        return vm::ReadError::negative_count;
    }
    if (static_cast<std::uint64_t>(count) > kMaxTriggers) {
        return vm::ReadError::trigger_count_out_of_range;
    }

    manager.triggers.assign(static_cast<std::size_t>(count), SimpleTrigger{});
    for (std::int32_t index = 0; index < count; ++index) {
        SimpleTrigger& trigger = manager.triggers[static_cast<std::size_t>(index)];
        const vm::ReadError error =
            seed_timer(trigger.timer, manager.defaultTimerNo, timersTotal);
        if (error != vm::ReadError::none) {
            return error;
        }
        const vm::ReadError readError = read_simple_trigger(reader, trigger, prefix, index);
        if (readError != vm::ReadError::none) {
            return readError;
        }
    }
    return vm::ReadError::none;
}

}  // namespace wse::game
