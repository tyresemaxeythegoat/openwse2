// Trigger records and their module-text readers.
//
// Evidence (Ghidra 12.1.4 pseudocode):
//   mbTriggerManager::readStream         0x140244990
//   mbTrigger::readStream                0x1402446c0
//   mbSimpleTriggerManager::readStream   0x14020ffe0
//   mbSimpleTrigger::readStream          0x14020fc30
//   mbGame::readTriggers                 0x1400cd080
//   mbGame::readSimpleTriggers           0x1400cd430
//
// Layouts (PDB):
//   mbTrigger             256 bytes type 0x078ba
//     +0x000 check/delay/rearm intervals (3 floats), +0x00c status
//     +0x010 conditions block, +0x070 consequences block
//     +0x0d0/+0x0e0/+0x0f0 check/delay/rearm rglAppTimer
//   mbSimpleTrigger       120 bytes type 0x07298
//     +0x000 interval, +0x008 statement block, +0x068 timer
//   rglTimer               16 bytes type 0x165cd
//     +0x000 m_timerNo int32, +0x008 m_value int64
//
// Both managers read a count and then construct that many entries, seeding
// every timer with m_defaultTimerNo and g_timersTotal[m_defaultTimerNo]
// (0x140620030, public symbol ?g_timersTotal@@3PA_KA).
//
// Runtime (mbTriggerManager::execute 0x140244e50, mbSimpleTrigger::execute
// 0x14020fd00, mbSimpleTriggerManager::execute 0x140210320):
//   status 0 idle, 1 waiting for the delay, 2 waiting for the rearm.
//   Intervals are seconds; timers advance in units of 100000 per second.
//   The int argument is a trigger event key, not a clock. Interval triggers
//   run for any non-negative key (mbGame::frameMove passes 1, mbMission::
//   frameMove passes 0x10). A check interval <= -0.1 (manager) or < -1.0
//   (simple trigger) instead matches that rounded interval against the key:
//   call sites pass -112 (updateExitStatus), -105/-106 (ranged attack),
//   -28 (die), -26 (knockdown), -30 (dropItem), etc. The float is rounded
//   with floor() and a strictly-greater-than-0.5 fraction bump.
//   mbTriggerManager::execute inlines the whole mbTrigger state machine; the
//   PDB declares mbTrigger::execute but no separate public symbol is emitted.
//
// Savegames (rglBinaryStream, vtable 0x14055d7b0):
//   mbTriggerManager::readSavegameStream  0x140244af0
//   mbTriggerManager::writeSavegameStream 0x140244ce0
//   mbSimpleTriggerManager::readSavegameStream 0x140210120
//   int32 trigger count, then per trigger int32 status and three int64 timer
//   values (check/delay/rearm); a simple trigger stores one int64 timer value.
//   Timer numbers are not saved. Extra file entries are read and discarded;
//   the retail reader shows a message box through rglError (0x140330210) and
//   then continues. rglBinaryStream stores raw little-endian values with no
//   per-value header (see wse/io/binary_stream.h).
//
// mbGame::readTriggers opens "triggers.txt", discards a three-token header
// ("triggersfile version 1" in the retail files), then calls the manager
// reader with the prefix "Game". readSimpleTriggers does the same with
// "simple_triggers.txt", "Simple", and mbSimpleTriggerManager. The prefixes
// become the block ids "Game Trigger [i] Conditions" and
// "Simple Trigger [i]".

#pragma once

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/io/binary_stream.h"
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

    // Shortest record: three intervals and two statement counts.
    manager.triggers.clear();
    manager.triggers.reserve(reader.plausible_count(static_cast<std::size_t>(count), 10));
    for (std::int32_t index = 0; index < count; ++index) {
        Trigger& trigger = manager.triggers.emplace_back();
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

    // Shortest record: one interval and one statement count.
    manager.triggers.clear();
    manager.triggers.reserve(reader.plausible_count(static_cast<std::size_t>(count), 4));
    for (std::int32_t index = 0; index < count; ++index) {
        SimpleTrigger& trigger = manager.triggers.emplace_back();
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

// ---------------------------------------------------------------------------
// Savegames (rglBinaryStream)
// ---------------------------------------------------------------------------
//
// mbTriggerManager::writeSavegameStream 0x140244ce0 writes:
//   int32 count
//   per trigger: int32 status, int64 check, delay, rearm timer values
// mbTriggerManager::readSavegameStream 0x140244af0 reads the same order. When
// the file count exceeds the manager count it reports the retail error
// "Load error: Number of triggers in file: %d, exceeds %d" and still consumes
// every file entry, discarding the surplus.
// mbSimpleTriggerManager::readSavegameStream 0x140210120 reads an int32 count
// and one int64 timer value per entry, with the analogous
// "Load error: Number of simple triggers in file: %d, exceeds %d" error.
// mbGame::writeStream (0x1400c9d05..0x1400c9d4a) writes the matching int32 count
// followed by each simple trigger's int64 timer value inline.

enum class SavegameResult : std::uint8_t {
    ok = 0,
    count_exceeded,
    negative_count,
    truncated,
};

[[nodiscard]] constexpr std::string_view describe(SavegameResult result) noexcept {
    switch (result) {
    case SavegameResult::ok:
        return "ok";
    case SavegameResult::count_exceeded:
        return "file record count exceeds the loaded manager";
    case SavegameResult::negative_count:
        return "negative record count";
    case SavegameResult::truncated:
        return "truncated binary savegame record";
    }
    return "unknown savegame result";
}

inline void write_trigger_savegame(const TriggerManager& manager, io::BinaryWriter& writer) {
    writer.write_i32(static_cast<std::int32_t>(manager.triggers.size()));
    for (const Trigger& trigger : manager.triggers) {
        writer.write_i32(trigger.status);
        writer.write_i64(trigger.checkTimer.value);
        writer.write_i64(trigger.delayTimer.value);
        writer.write_i64(trigger.rearmTimer.value);
    }
}

// Applies status and timer values to the existing manager entries; timer
// numbers and statement blocks are not part of the savegame.
[[nodiscard]] inline SavegameResult read_trigger_savegame(TriggerManager& manager,
                                                          io::BinaryReader& reader) {
    std::int32_t fileCount = 0;
    if (!reader.read_i32(fileCount)) {
        return SavegameResult::truncated;
    }
    if (fileCount < 0) {
        return SavegameResult::negative_count;
    }

    const std::uint32_t count = static_cast<std::uint32_t>(fileCount);
    const std::size_t available = manager.triggers.size();
    const bool exceeded = count > available;

    for (std::uint32_t index = 0; index < count; ++index) {
        std::int32_t status = 0;
        std::int64_t check = 0;
        std::int64_t delay = 0;
        std::int64_t rearm = 0;
        if (!reader.read_i32(status) || !reader.read_i64(check) || !reader.read_i64(delay) ||
            !reader.read_i64(rearm)) {
            return SavegameResult::truncated;
        }
        if (index < available) {
            Trigger& trigger = manager.triggers[index];
            trigger.status = status;
            trigger.checkTimer.value = check;
            trigger.delayTimer.value = delay;
            trigger.rearmTimer.value = rearm;
        }
    }

    return exceeded ? SavegameResult::count_exceeded : SavegameResult::ok;
}

inline void write_simple_trigger_savegame(const SimpleTriggerManager& manager,
                                          io::BinaryWriter& writer) {
    writer.write_i32(static_cast<std::int32_t>(manager.triggers.size()));
    for (const SimpleTrigger& trigger : manager.triggers) {
        writer.write_i64(trigger.timer.value);
    }
}

[[nodiscard]] inline SavegameResult read_simple_trigger_savegame(SimpleTriggerManager& manager,
                                                                 io::BinaryReader& reader) {
    std::int32_t fileCount = 0;
    if (!reader.read_i32(fileCount)) {
        return SavegameResult::truncated;
    }
    if (fileCount < 0) {
        return SavegameResult::negative_count;
    }

    const std::uint32_t count = static_cast<std::uint32_t>(fileCount);
    const std::size_t available = manager.triggers.size();
    const bool exceeded = count > available;

    for (std::uint32_t index = 0; index < count; ++index) {
        std::int64_t value = 0;
        if (!reader.read_i64(value)) {
            return SavegameResult::truncated;
        }
        if (index < available) {
            manager.triggers[index].timer.value = value;
        }
    }

    return exceeded ? SavegameResult::count_exceeded : SavegameResult::ok;
}

// ---------------------------------------------------------------------------
// Runtime state machines
// ---------------------------------------------------------------------------

inline constexpr float kManagerEventThreshold = -0.1F;
inline constexpr float kSimpleEventThreshold = -1.0F;
inline constexpr double kTimerTicksPerSecond = 100000.0;

// The retail loops take floor(interval), then add one only when the fraction
// is strictly above 0.5. Exact halves therefore round toward minus infinity.
// The result is compared with a negative trigger event key.
[[nodiscard]] inline int interval_key(float interval) noexcept {
    const float base = std::floor(interval);
    int key = static_cast<int>(base);
    if (interval - base > 0.5F) {
        ++key;
    }
    return key;
}

// (float)(now - saved) / 100000.0, preserving the retail cast order.
[[nodiscard]] inline double timer_elapsed(std::int64_t now, std::int64_t saved) noexcept {
    return static_cast<double>(static_cast<float>(now - saved)) / kTimerTicksPerSecond;
}

[[nodiscard]] inline bool timer_now(std::span<const std::int64_t> timersTotal,
                                    const AppTimer& timer,
                                    std::int64_t& out) noexcept {
    if (timer.timerNo < 0 || static_cast<std::size_t>(timer.timerNo) >= timersTotal.size()) {
        return false;
    }
    out = timersTotal[static_cast<std::size_t>(timer.timerNo)];
    return true;
}

struct TriggerCallbacks {
    bool (*checkConditions)(void* context, const vm::StatementBlock& block) = nullptr;
    void (*runConsequences)(void* context, const vm::StatementBlock& block) = nullptr;
    void* context = nullptr;
};

struct SimpleTriggerCallbacks {
    void (*runBlock)(void* context, const vm::StatementBlock& block) = nullptr;
    void* context = nullptr;
};

// One iteration of the inlined mbTrigger state machine. Returns false when a
// timer number is out of range; the retail code reads past g_timersTotal.
[[nodiscard]] inline bool execute_trigger(Trigger& trigger,
                                          std::int32_t eventKey,
                                          std::span<const std::int64_t> timersTotal,
                                          const TriggerCallbacks& callbacks) {
    if (callbacks.checkConditions == nullptr || callbacks.runConsequences == nullptr) {
        return false;
    }

    std::int64_t now = 0;

    if (trigger.status == 2) {
        if (!timer_now(timersTotal, trigger.rearmTimer, now)) {
            return false;
        }
        if (trigger.rearmInterval <= timer_elapsed(now, trigger.rearmTimer.value)) {
            trigger.status = 0;
            if (!timer_now(timersTotal, trigger.checkTimer, now)) {
                return false;
            }
            trigger.checkTimer.value = now;
        }
    }

    if (trigger.status == 0) {
        if (trigger.checkInterval <= kManagerEventThreshold) {
            if (interval_key(trigger.checkInterval) == eventKey &&
                callbacks.checkConditions(callbacks.context, trigger.conditions)) {
                trigger.status = 2;
                if (!timer_now(timersTotal, trigger.rearmTimer, now)) {
                    return false;
                }
                trigger.rearmTimer.value = now;
                callbacks.runConsequences(callbacks.context, trigger.consequences);
            }
        } else if (eventKey >= 0) {
            if (!timer_now(timersTotal, trigger.checkTimer, now)) {
                return false;
            }
            if (trigger.checkInterval <= timer_elapsed(now, trigger.checkTimer.value)) {
                trigger.checkTimer.value = now;
                if (callbacks.checkConditions(callbacks.context, trigger.conditions)) {
                    trigger.status = 1;
                    if (!timer_now(timersTotal, trigger.delayTimer, now)) {
                        return false;
                    }
                    trigger.delayTimer.value = now;
                }
            }
        }
    }

    if (trigger.status == 1) {
        if (!timer_now(timersTotal, trigger.delayTimer, now)) {
            return false;
        }
        if (trigger.delayInterval <= timer_elapsed(now, trigger.delayTimer.value)) {
            trigger.status = 2;
            if (!timer_now(timersTotal, trigger.rearmTimer, now)) {
                return false;
            }
            trigger.rearmTimer.value = now;
            callbacks.runConsequences(callbacks.context, trigger.consequences);
        }
    }

    return true;
}

[[nodiscard]] inline bool execute_trigger_manager(
    TriggerManager& manager,
    std::int32_t eventKey,
    std::span<const std::int64_t> timersTotal,
    const TriggerCallbacks& callbacks) {
    for (auto& trigger : manager.triggers) {
        if (!execute_trigger(trigger, eventKey, timersTotal, callbacks)) {
            return false;
        }
    }
    return true;
}

// A simple trigger has one block, executed when its interval elapses. The
// retail execute ignores the block result. mbSimpleTriggerManager::execute
// additionally aborts its loop after an odd event key when a game event
// queue transitions; that check needs mbGame state and is not modelled here.
[[nodiscard]] inline bool execute_simple_trigger(
    SimpleTrigger& trigger,
    std::int32_t eventKey,
    std::span<const std::int64_t> timersTotal,
    const SimpleTriggerCallbacks& callbacks) {
    if (callbacks.runBlock == nullptr) {
        return false;
    }

    if (trigger.interval < kSimpleEventThreshold) {
        if (interval_key(trigger.interval) != eventKey) {
            return true;
        }
    } else {
        if (eventKey < 0) {
            return true;
        }
        std::int64_t now = 0;
        if (!timer_now(timersTotal, trigger.timer, now)) {
            return false;
        }
        if (timer_elapsed(now, trigger.timer.value) < trigger.interval) {
            return true;
        }
        trigger.timer.value = now;
    }

    callbacks.runBlock(callbacks.context, trigger.block);
    return true;
}

[[nodiscard]] inline bool execute_simple_trigger_manager(
    SimpleTriggerManager& manager,
    std::int32_t eventKey,
    std::span<const std::int64_t> timersTotal,
    const SimpleTriggerCallbacks& callbacks) {
    for (auto& trigger : manager.triggers) {
        if (!execute_simple_trigger(trigger, eventKey, timersTotal, callbacks)) {
            return false;
        }
    }
    return true;
}

}  // namespace wse::game
