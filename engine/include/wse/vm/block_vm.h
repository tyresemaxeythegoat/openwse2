// Portable statement-block interpreter and trigger/script execution bridge.
//
// Evidence (WSE2 v1.1.5.5 PE + Ghidra 12.1.4 REA pseudocode + PDB TPI):
//   mbStatement::parseOperand      0x1401c61f0  operand decode + fallback tags
//   mbStatement::returnValue       0x1401c6280  register/slot/local lvalue store
//   mbStatement::execute           0x1401c62d0  this_or_next / neg / dispatch
//   mbStatementBlock::analyze      0x1401f68c0  two-stack block endStatement + locals
//   mbStatementBlock::execute      0x1401f6b50  call stack (257), try/else, params
//   mbStatementBlock::startLoop    0x1401f7410  loop frame push + initial write
//   mbStatementBlock::endLoop      0x1401f7a90  loop step/pop + register advance
//   mbStatementBlock::canLoop      0x1401f8080  try_for_range[_backwards] bounds
//   mbTriggerManager::execute      0x140244e50  trigger condition/consequence VM
//   mbSimpleTrigger::execute       0x14020fd00  simple-trigger VM dispatch
//   mbSimpleTriggerManager::execute 0x140210320 simple-trigger manager loop
//   mbGame::executeScript          0x1400dbf40  script caller = 0x200000
//   mbGame::executeMappedScript    0x1400dbfd0  mapped script lookup
//   Layouts: mbBasicGame (0xadd3, 221616B), mbCallStack (0x715b, 3096B),
//            mbLoop (0x1f89a, 56B), mbLoopManager (0x1eb6b, 7176B).
//
// Key structural facts verified directly against v1.1.5.5:
//   - mbBasicGame +0x270 (0x140663c60) is the 128-entry int64 register array
//     accessed by operand tag 1; +0x258 (0x140663c48) is mbSlots m_globalVariables
//     accessed by operand tag 2; frame locals are accessed by operand tag 0x11.
//   - mbStatement::parseOperand returns payload for unknown tags (such as tag 3
//     and tag 0x16 string operands) and forwards the tag byte to executeWSE.
//   - mbStatementBlock::analyze keeps two 128-entry stacks (tryStack updated on
//     else_try; startStack kept at the opening statement) so nested statements
//     with unresolved endStatement link to the next else_try or try_end, and
//     closing try_end fills the opener/else_try and trailing branch statements.
//   - mbStatementBlock::execute resets g_skipStatement on entry, checks recursion
//     depth < 256 (0..255), isolates 1024 int64 locals and 128 loop frames per
//     depth, and aborts the block immediately on top-level failure (numLoops==0).
//
// Safety deviation: retail indexes m_registers (128) and stack locals (1024)
// without bounds checking and logs through rglError. This port bounds-checks
// register/local/stack capacities and records errors on VmContext without
// out-of-bounds memory access.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/game/script.h"
#include "wse/game/trigger.h"
#include "wse/game/world_state.h"
#include "wse/vm/execute_wse.h"
#include "wse/vm/lua_runtime.h"
#include "wse/vm/operand.h"
#include "wse/vm/slots.h"
#include "wse/vm/statement.h"

namespace wse::vm {

inline constexpr std::uint32_t kThisOrNextFlag = 0x40000000U;
inline constexpr std::uint32_t kNegFlag = 0x80000000U;
inline constexpr std::uint32_t kOpcodeMask = 0x0FFFFFFFU;

inline constexpr std::uint32_t kNativeOpcodeMin = 30U;
inline constexpr std::uint32_t kNativeOpcodeMax = 2767U;
inline constexpr std::uint32_t kWseOpcodeMin = 2768U;

inline constexpr std::int32_t kMaxCallDepth = 255;       // 0x1401f71d1 (< 256)
inline constexpr std::size_t kMaxBlockStackDepth = 128;  // 0x1401f6936
inline constexpr std::size_t kMinLocalVariables = 32;    // 0x1401f6ae5
inline constexpr std::int32_t kScriptCaller = 0x200000;  // 0x1400dbf40
inline constexpr std::int64_t kDivisionByZeroSentinel = 40000000LL;  // 0x1401dd030

namespace core_op {
inline constexpr std::uint32_t kCallScript = 1U;
inline constexpr std::uint32_t kTryEnd = 3U;
inline constexpr std::uint32_t kTryBegin = 4U;
inline constexpr std::uint32_t kElseTry = 5U;
inline constexpr std::uint32_t kTryForRange = 6U;
inline constexpr std::uint32_t kTryForRangeBackwards = 7U;
inline constexpr std::uint32_t kBreakLoop = 8U;
inline constexpr std::uint32_t kContinueLoop = 9U;
inline constexpr std::uint32_t kTryForParties = 0xBU;
inline constexpr std::uint32_t kTryForAgents = 0xCU;
inline constexpr std::uint32_t kTryForPropInstances = 0x10U;
inline constexpr std::uint32_t kTryForPlayers = 0x11U;
inline constexpr std::uint32_t kTryForDictKeys = 0x12U;
inline constexpr std::uint32_t kStoreScriptParam1 = 21U;
inline constexpr std::uint32_t kStoreScriptParam2 = 22U;
inline constexpr std::uint32_t kStoreScriptParam = 23U;

inline constexpr std::uint32_t kGe = 30U;
inline constexpr std::uint32_t kEq = 31U;
inline constexpr std::uint32_t kGt = 32U;
inline constexpr std::uint32_t kIsBetween = 33U;
inline constexpr std::uint32_t kValLshift = 2100U;
inline constexpr std::uint32_t kValRshift = 2101U;
inline constexpr std::uint32_t kValAdd = 2105U;
inline constexpr std::uint32_t kValSub = 2106U;
inline constexpr std::uint32_t kValMul = 2107U;
inline constexpr std::uint32_t kValDiv = 2108U;
inline constexpr std::uint32_t kValMod = 2109U;
inline constexpr std::uint32_t kValMin = 2110U;
inline constexpr std::uint32_t kValMax = 2111U;
inline constexpr std::uint32_t kValClamp = 2112U;
inline constexpr std::uint32_t kValAbs = 2113U;
inline constexpr std::uint32_t kValOr = 2114U;
inline constexpr std::uint32_t kValAnd = 2115U;
inline constexpr std::uint32_t kStoreOr = 2116U;
inline constexpr std::uint32_t kStoreAnd = 2117U;
inline constexpr std::uint32_t kStoreMod = 2119U;
inline constexpr std::uint32_t kStoreAdd = 2120U;
inline constexpr std::uint32_t kStoreSub = 2121U;
inline constexpr std::uint32_t kStoreMul = 2122U;
inline constexpr std::uint32_t kStoreDiv = 2123U;
inline constexpr std::uint32_t kSetFixedPointMultiplier = 2124U;
inline constexpr std::uint32_t kStoreSqrt = 2125U;
inline constexpr std::uint32_t kStorePow = 2126U;
inline constexpr std::uint32_t kStoreSin = 2127U;
inline constexpr std::uint32_t kStoreCos = 2128U;
inline constexpr std::uint32_t kStoreTan = 2129U;
inline constexpr std::uint32_t kConvertToFixedPoint = 2130U;
inline constexpr std::uint32_t kConvertFromFixedPoint = 2131U;
inline constexpr std::uint32_t kAssign = 2133U;
}  // namespace core_op

struct VmContext;

enum class NativeDispatch : std::uint8_t {
    handled = 0,       // handled statement or passing condition
    condition_false,   // handled condition evaluated false
    unimplemented,     // native opcode not yet implemented
};

[[nodiscard]] constexpr std::string_view describe(NativeDispatch result) noexcept {
    switch (result) {
    case NativeDispatch::handled:
        return "handled";
    case NativeDispatch::condition_false:
        return "false condition";
    case NativeDispatch::unimplemented:
        return "unimplemented native opcode";
    }
    return "unknown native dispatch result";
}

using NativeOpHook = bool (*)(void* user,
                              std::uint32_t opcode,
                              std::span<const std::int64_t> wide,
                              std::span<const std::int32_t> narrow,
                              std::span<const std::uint8_t> tags,
                              const Statement& statement,
                              VmContext& ctx,
                              std::span<std::int64_t> locals,
                              bool& handled);

using VmErrorCallback = void (*)(void* user, std::string_view message);

struct LoopFrame {
    std::int32_t statementNo = 0;
    std::int32_t value = 0;
    // try_for_dict_keys cursor. Safety deviation: the frame holds the object
    // id and the current key instead of a pointer/iterator into the map, and
    // re-resolves the dictionary on every step, so a loop body that frees,
    // clears, or erases from the dictionary cannot leave a dangling cursor.
    std::int32_t dictId = -1;
    std::string dictKey{};
};

struct VmContext {
    // Tag 1 operands and WSE register_{get,set} share mbBasicGame::m_registers.
    std::array<std::int64_t, 128> registers{};
    // Tag 2 operands map to mbBasicGame::m_globalVariables (mbSlots).
    Slots globalVariables;
    std::array<float, 128> fpRegisters{};
    std::array<std::u16string, 128> stringRegisters{};
    std::span<const std::u16string_view> globalStrings{};
    std::span<const std::u16string_view> localStrings{};

    std::array<std::int64_t, 16> returnValues{};
    std::int32_t numReturnValues = 0;
    std::int32_t fixedPointMultiplier = 100;
    double appTimeSeconds = 0.0;
    bool hasMissionTemplate = false;
    std::int32_t missionTemplateNo = -1;
    WseOptionsState options{};
    std::array<WseTransform, 128> positions{};
    std::array<WseAppTimer, 128> appTimers{};
    std::array<std::int64_t, 3> timersTotal{};
    WseMersenneTwister mtRng{};
    std::int32_t randSeed = 0;
    bool profilerRecording = false;
    std::vector<std::string> profilerMarkers{};
    WseDictionaryStore dictionaries{};
    wse::game::WseWorldState world{};

    // 0x1401f6dd9 checks rglConfig::General::bLocalVariablesZeroInitialization
    // (loaded at 0x14026f896 via rglIni::getBool 0x1402a8740; default true).
    bool clearLocalsBetweenCalls = true;
    std::int32_t maxCallDepth = kMaxCallDepth;
    std::int32_t currentDepth = 0;
    // Statement blocks currently on the C++ stack. A script started while one is running (a
    // native handler calling `execute_mapped_script`) gets its own frame at `currentDepth + 1`;
    // retail allocates locals per call, so it never shares the caller's.
    std::int32_t activeBlocks = 0;
    std::int32_t currentCaller = 0;
    bool skipStatement = false;
    game::ScriptManager* scripts = nullptr;
    std::shared_ptr<WseLuaRuntime> luaRuntime{};
    // Number of trigger state machines currently on the call stack. While it
    // is non-zero the Lua runtime queues structural trigger/presentation edits
    // (game.addTrigger, game.removeTrigger, ...) instead of resizing a vector
    // that is being iterated; see TriggerIterationScope.
    std::int32_t triggerIterationDepth = 0;

    NativeOpHook nativeHook = nullptr;
    void* nativeHookUser = nullptr;

    VmErrorCallback onError = nullptr;
    void* errorUser = nullptr;
    std::size_t errorCount = 0;
    std::string lastError;
    std::size_t unimplementedCount = 0;
    std::uint32_t lastUnimplementedOpcode = 0;
    std::map<std::uint32_t, std::size_t> unimplementedOpcodeCounts{};
    bool failOnUnimplemented = false;
    std::int32_t maxObservedDepth = -1;

    // Heap-backed per-depth arenas mirror retail's dynamic local/loop pools
    // while preventing deep 255-level call_script stacks from overflowing.
    std::vector<std::array<std::int64_t, kDefaultLocalVariables>> localFrames;
    std::vector<std::array<LoopFrame, kMaxBlockStackDepth>> loopFrames;

    void report_error(std::string_view message) {
        ++errorCount;
        lastError.assign(message);
        if (onError != nullptr) {
            onError(errorUser, message);
        }
    }

    void record_unimplemented(std::uint32_t opcode) {
        ++unimplementedCount;
        lastUnimplementedOpcode = opcode;
        ++unimplementedOpcodeCounts[opcode];
        if (failOnUnimplemented) {
            report_error("Unimplemented opcode " + std::to_string(opcode));
        }
    }
};

// Floor-with-half-up rounding shared by retail trigonometric/math helpers
// (0x1402a1b80: floorf(x), ++rax iff x - floorf(x) > 0.5f).
[[nodiscard]] inline std::int64_t f2i_round(float value) noexcept {
    const float base = std::floor(value);
    auto out = static_cast<std::int64_t>(base);
    if (value - base > 0.5F) {
        ++out;
    }
    return out;
}

[[nodiscard]] constexpr bool is_block_opener(std::uint32_t opcode) noexcept {
    switch (opcode) {
    case core_op::kTryBegin:
    case core_op::kTryForRange:
    case core_op::kTryForRangeBackwards:
    case core_op::kTryForAgents:
    case core_op::kTryForParties:
    case core_op::kTryForPropInstances:
    case core_op::kTryForPlayers:
    case core_op::kTryForDictKeys:
        return true;
    default:
        return false;
    }
}

// Retail safe-divide helper matching signed two's-complement division on x86.
[[nodiscard]] inline std::int64_t checked_signed_div(std::int64_t lhs,
                                                     std::int64_t rhs,
                                                     VmContext& ctx) {
    if (rhs == 0) {
        ctx.report_error("Division by zero");
        return kDivisionByZeroSentinel;
    }
    if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return lhs / rhs;
}

[[nodiscard]] inline std::int64_t checked_signed_mod(std::int64_t lhs,
                                                     std::int64_t rhs,
                                                     VmContext& ctx,
                                                     bool& ok) {
    if (rhs == 0) {
        ctx.report_error("Modulo by zero");
        ok = false;
        return 0;
    }
    ok = true;
    if (lhs == std::numeric_limits<std::int64_t>::min() && rhs == -1) {
        return 0;
    }
    return lhs % rhs;
}

// Decodes one statement operand (0x1401c61f0). Values <= 0 are signed 64-bit
// immediates; positive values extract tag (bits 56..62) and 56-bit payload.
// Tags other than 0, 1, 2, 0x11 return the raw payload and expose `outTag` for
// downstream WSE string/object handlers.
[[nodiscard]] inline std::int64_t parse_operand(std::uint64_t raw,
                                                VmContext& ctx,
                                                std::span<const std::int64_t> locals,
                                                std::uint8_t& outTag) {
    if (static_cast<std::int64_t>(raw) <= 0) {
        outTag = 0;
        return static_cast<std::int64_t>(raw);
    }

    outTag = static_cast<std::uint8_t>((raw >> 56) & kTagMask);
    const std::uint64_t payload = raw & kPayloadMask;
    const auto index = static_cast<std::int32_t>(payload);

    switch (static_cast<OperandTag>(outTag)) {
    case OperandTag::immediate:
        return static_cast<std::int64_t>(payload);
    case OperandTag::global:
        if (index < 0 || static_cast<std::size_t>(index) >= ctx.registers.size()) {
            ctx.report_error("Register index out of range in parse_operand");
            return 0;
        }
        return ctx.registers[static_cast<std::size_t>(index)];
    case OperandTag::slot:
        return ctx.globalVariables.get(index);
    case OperandTag::local:
        if (index < 0 || static_cast<std::size_t>(index) >= locals.size()) {
            ctx.report_error("Local variable index out of range in parse_operand");
            return 0;
        }
        return locals[static_cast<std::size_t>(index)];
    }

    return static_cast<std::int64_t>(payload);
}

// Writes `value` into a statement's lvalue operand (0x1401c6280).
inline bool store_lvalue(std::uint64_t raw,
                         std::int64_t value,
                         VmContext& ctx,
                         std::span<std::int64_t> locals) {
    const auto tag = tag_of(raw);
    const auto index = static_cast<std::int32_t>(raw & kPayloadMask);

    switch (static_cast<OperandTag>(tag)) {
    case OperandTag::global:
        if (index < 0 || static_cast<std::size_t>(index) >= ctx.registers.size()) {
            ctx.report_error("Invalid Register ID in store_lvalue");
            return false;
        }
        ctx.registers[static_cast<std::size_t>(index)] = value;
        return true;
    case OperandTag::slot:
        if (!ctx.globalVariables.set(index, value)) {
            ctx.report_error("Negative global variable index in store_lvalue");
            return false;
        }
        return true;
    case OperandTag::local:
        if (index < 0 || static_cast<std::size_t>(index) >= locals.size()) {
            ctx.report_error("Local variable index out of range in store_lvalue");
            return false;
        }
        locals[static_cast<std::size_t>(index)] = value;
        return true;
    case OperandTag::immediate:
        break;
    }

    ctx.report_error("Illegal lvalue in statement");
    return false;
}

// Mirrors mbStatementBlock::analyze (0x1401f68c0): pairs try/loop openers with
// else_try (5) and try_end (3), links inner failing statements' endStatement to
// the enclosing branch target, and estimates local storage in [32, 1024].
inline void analyze_statement_block(StatementBlock& block, VmContext* ctx = nullptr) {
    if (block.analyzed) {
        return;
    }

    const auto total = static_cast<std::int32_t>(block.statements.size());
    std::array<std::int32_t, kMaxBlockStackDepth> tryStack{};
    std::array<std::int32_t, kMaxBlockStackDepth> startStack{};
    std::size_t sp = 0;

    for (std::int32_t i = 0; i < total; ++i) {
        const std::uint32_t op = block.statements[static_cast<std::size_t>(i)].opcode & kOpcodeMask;
        if (is_block_opener(op)) {
            if (sp < kMaxBlockStackDepth) {
                tryStack[sp] = i;
                startStack[sp] = i;
                ++sp;
            } else if (ctx != nullptr) {
                ctx->report_error("Statement block nesting depth exceeds 128");
            }
        } else if (op == core_op::kElseTry) {
            if (sp > 0) {
                const std::int32_t start = tryStack[sp - 1];
                for (std::int32_t k = start + 1; k < i; ++k) {
                    auto& inner = block.statements[static_cast<std::size_t>(k)];
                    if (inner.endStatement < 0) {
                        inner.endStatement = i;
                    }
                }
                tryStack[sp - 1] = i;
            } else if (ctx != nullptr) {
                ctx->report_error("Unmatched else_try in statement block");
            }
        } else if (op == core_op::kTryEnd) {
            if (sp > 0) {
                const std::int32_t start = startStack[sp - 1];
                for (std::int32_t k = start; k < i; ++k) {
                    auto& inner = block.statements[static_cast<std::size_t>(k)];
                    if (inner.endStatement < 0) {
                        inner.endStatement = i;
                    }
                }
                --sp;
            } else if (ctx != nullptr) {
                ctx->report_error("Unmatched try_end in statement block");
            }
        }
    }

    for (auto& stmt : block.statements) {
        if (stmt.endStatement < 0) {
            stmt.endStatement = total;
        }
    }

    std::int32_t maxLocal = 31;
    for (const auto& stmt : block.statements) {
        const std::int32_t count =
            std::clamp(stmt.numOperands, 0, static_cast<std::int32_t>(kMaxOperands));
        for (std::int32_t opIndex = 0; opIndex < count; ++opIndex) {
            const auto raw = static_cast<std::uint64_t>(stmt.operands[opIndex]);
            if (static_cast<std::int64_t>(raw) > 0 &&
                tag_of(raw) == static_cast<std::uint8_t>(OperandTag::local)) {
                const auto localIdx = static_cast<std::int32_t>(raw & kPayloadMask);
                if (localIdx > maxLocal) {
                    maxLocal = localIdx;
                }
            }
        }
    }

    std::int32_t used = maxLocal + 1;
    if (used < static_cast<std::int32_t>(kMinLocalVariables)) {
        used = static_cast<std::int32_t>(kMinLocalVariables);
    } else if (used > kDefaultLocalVariables) {
        used = kDefaultLocalVariables;
    }
    block.numLocalVariablesUsed = used;
    block.analyzed = true;
}

[[nodiscard]] inline NativeDispatch dispatch_native_statement(
    std::uint32_t opcode,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals);

[[nodiscard]] inline bool wse_invoke_script_callback(
    void* user,
    std::int32_t script_index,
    std::span<const std::int64_t> params);

[[nodiscard]] inline bool dispatch_wse_statement(
    std::uint32_t opcode,
    std::int32_t caller,
    std::size_t count,
    std::span<const std::int64_t> wide,
    std::span<const std::int32_t> narrow,
    std::span<const std::uint8_t> tags,
    const Statement& statement,
    VmContext& ctx,
    std::span<std::int64_t> locals) {
    WseOperands operands;
    operands.wide = wide.first(count);
    operands.narrow = narrow.first(count);
    operands.tags = tags.first(count);
    operands.fp = ctx.fpRegisters;
    operands.registers = ctx.registers;
    operands.state.caller = caller;
    operands.state.return_values = ctx.returnValues;
    operands.state.num_return_values = ctx.numReturnValues;
    operands.state.app_time_seconds = ctx.appTimeSeconds;
    operands.state.current_mission_template_no =
        ctx.hasMissionTemplate ? &ctx.missionTemplateNo : nullptr;
    operands.state.strings.registers = ctx.stringRegisters;
    operands.state.strings.global = ctx.globalStrings;
    operands.state.strings.local = ctx.localStrings;
    operands.state.options = &ctx.options;
    operands.state.return_values_out = ctx.returnValues;
    operands.state.num_return_values_out = &ctx.numReturnValues;
    operands.state.fixed_point_multiplier = ctx.fixedPointMultiplier;
    operands.state.positions = ctx.positions;
    operands.state.timers = ctx.appTimers;
    operands.state.timers_total = ctx.timersTotal;
    operands.state.rng = &ctx.mtRng;
    operands.state.rand_seed = &ctx.randSeed;
    operands.state.profiler_recording = &ctx.profilerRecording;
    operands.state.profiler_markers = &ctx.profilerMarkers;
    operands.state.dictionaries = &ctx.dictionaries;
    operands.state.lua_runtime = ctx.luaRuntime.get();
    operands.state.world = &ctx.world;
    if (ctx.scripts != nullptr) {
        operands.state.num_scripts = ctx.scripts->scripts.size();
        operands.state.execute_script_fn = &wse_invoke_script_callback;
        operands.state.execute_script_user = &ctx;
    }

    WseEffect effect{};
    const WseDispatch dispatch = execute_wse(opcode, operands, effect);
    if (dispatch == WseDispatch::unimplemented) {
        ctx.record_unimplemented(opcode);
        return !ctx.failOnUnimplemented;
    }
    if (dispatch == WseDispatch::operand_count_out_of_range) {
        ctx.report_error("WSE opcode operand count out of range");
        return true;
    }
    if (dispatch == WseDispatch::handled && effect.store_value && count > 0) {
        store_lvalue(static_cast<std::uint64_t>(statement.operands[0]), effect.value, ctx, locals);
    }
    return dispatch != WseDispatch::condition_false;
}

[[nodiscard]] constexpr NativeDispatch dispatch_native_non_switch(
    std::uint32_t opcode) noexcept {
    // Retail v1.1.5.5 non-switch runtime opcodes outside mbStatement::execute's
    // switch table: 473/477/1070/1104/1302/1491 fall through to bResult=1;
    // 1004 falls into execute_wse and returns false when unhandled.
    switch (opcode) {
    case 473U:
    case 477U:
    case 1070U:
    case 1104U:
    case 1302U:
    case 1491U:
        return NativeDispatch::handled;
    case 1004U:
        return NativeDispatch::condition_false;
    default:
        return NativeDispatch::unimplemented;
    }
}

// Executes one leaf statement (>= 30), honoring g_skipStatement (0x40000000
// this_or_next) and negation (0x80000000 neg). Mirrors 0x1401c62d0.
[[nodiscard]] inline bool execute_statement(const Statement& statement,
                                            std::int32_t caller,
                                            VmContext& ctx,
                                            std::span<std::int64_t> locals,
                                            bool allowLuaHook = true) {
    const bool thisOrNext = (statement.opcode & kThisOrNextFlag) != 0;
    const bool neg = (statement.opcode & kNegFlag) != 0;
    const std::uint32_t opcode = statement.opcode & kOpcodeMask;

    if (ctx.skipStatement) {
        if (!thisOrNext) {
            ctx.skipStatement = false;
        }
        return true;
    }

    const std::size_t count =
        static_cast<std::size_t>(std::clamp(statement.numOperands, 0, static_cast<std::int32_t>(kMaxOperands)));
    std::array<std::int64_t, kMaxOperands> wide{};
    std::array<std::int32_t, kMaxOperands> narrow{};
    std::array<std::uint8_t, kMaxOperands> tags{};

    // First 5 slots start zeroed (0x1401c6399-0x1401c63ec); remaining slots stay zero too.
    for (std::size_t i = 0; i < count; ++i) {
        const std::int64_t value =
            parse_operand(static_cast<std::uint64_t>(statement.operands[i]), ctx, locals, tags[i]);
        wide[i] = value;
        narrow[i] = static_cast<std::int32_t>(value);
    }

    if (allowLuaHook && ctx.luaRuntime != nullptr && ctx.luaRuntime->is_ready() &&
        ctx.luaRuntime->has_operation_hook(opcode)) {
        const auto hookRes = ctx.luaRuntime->on_operation_execute(
            opcode,
            std::span<const std::int64_t>(wide.data(), count),
            std::span<const std::uint8_t>(tags.data(), count),
            ctx);
        if (!hookRes.execute) {
            if (hookRes.has_lhs && count > 0) {
                store_lvalue(static_cast<std::uint64_t>(statement.operands[0]),
                             hookRes.lhs_value, ctx, locals);
            }
            bool hookResult = hookRes.has_cf ? hookRes.cf_result : true;
            if (neg) {
                hookResult = !hookResult;
            }
            if (thisOrNext) {
                if (hookResult) {
                    ctx.skipStatement = true;
                } else {
                    hookResult = true;
                }
            }
            return hookResult;
        }
    }

    bool result = true;
    if (opcode >= kWseOpcodeMin) {
        const std::size_t unimplementedBefore = ctx.unimplementedCount;
        result = dispatch_wse_statement(opcode, caller, count, wide, narrow, tags, statement, ctx, locals);
        if (ctx.failOnUnimplemented && ctx.unimplementedCount > unimplementedBefore) {
            return false;
        }
    } else if (opcode >= kNativeOpcodeMin && opcode <= kNativeOpcodeMax) {
        const std::int32_t savedCaller = ctx.currentCaller;
        ctx.currentCaller = caller;
        NativeDispatch nativeResult =
            dispatch_native_statement(opcode, wide, narrow, tags, statement, ctx, locals);
        ctx.currentCaller = savedCaller;
        if (nativeResult == NativeDispatch::unimplemented) {
            nativeResult = dispatch_native_non_switch(opcode);
        }
        if (nativeResult == NativeDispatch::unimplemented) {
            ctx.record_unimplemented(opcode);
            if (ctx.failOnUnimplemented) {
                return false;
            }
            result = true;
        } else {
            result = (nativeResult == NativeDispatch::handled);
        }
    }

    if (neg) {
        result = !result;
    }
    if (thisOrNext) {
        if (result) {
            ctx.skipStatement = true;
        } else {
            result = true;
        }
    }
    return result;
}

inline void set_dict_loop_key(const Statement& stmt,
                              const LoopFrame& frame,
                              VmContext& ctx,
                              std::span<const std::int64_t> locals) {
    if (stmt.numOperands <= 0 || frame.value == -1) {
        return;
    }
    std::uint8_t unusedTag = 0;
    const auto sregIdx = static_cast<std::int32_t>(
        parse_operand(static_cast<std::uint64_t>(stmt.operands[0]), ctx, locals, unusedTag));
    if (!wse_register_ok(sregIdx)) {
        ctx.report_error("Invalid string register index in try_for_dict_keys");
        return;
    }
    ctx.stringRegisters[static_cast<std::size_t>(sregIdx)] =
        wse_utf8_to_utf16(frame.dictKey);
}

[[nodiscard]] inline std::int32_t find_next_valid_party(const VmContext& ctx,
                                                        std::int32_t start) noexcept;
[[nodiscard]] inline std::int32_t find_next_valid_agent(const Statement& stmt,
                                                        VmContext& ctx,
                                                        std::span<const std::int64_t> locals,
                                                        std::int32_t start,
                                                        bool reportPosError);
[[nodiscard]] inline std::int32_t find_next_valid_prop(const Statement& stmt,
                                                       VmContext& ctx,
                                                       std::span<const std::int64_t> locals,
                                                       std::int32_t start);
[[nodiscard]] inline std::int32_t find_next_valid_player(const VmContext& ctx,
                                                         std::int32_t start) noexcept;

[[nodiscard]] inline bool can_loop(const StatementBlock& block,
                                   VmContext& ctx,
                                   std::span<const std::int64_t> locals,
                                   std::span<const LoopFrame> loops,
                                   std::int32_t numLoops) {
    if (numLoops <= 0) {
        return false;
    }
    const LoopFrame& top = loops[static_cast<std::size_t>(numLoops - 1)];
    const Statement& stmt = block.statements[static_cast<std::size_t>(top.statementNo)];
    const std::uint32_t opcode = stmt.opcode & kOpcodeMask;
    std::uint8_t unusedTag = 0;

    switch (opcode) {
    case core_op::kTryForRange: {
        const std::int64_t upper =
            stmt.numOperands > 2 ? parse_operand(static_cast<std::uint64_t>(stmt.operands[2]),
                                                 ctx, locals, unusedTag)
                                 : 0;
        return static_cast<std::int64_t>(top.value) < upper;
    }
    case core_op::kTryForRangeBackwards: {
        const std::int64_t lower =
            stmt.numOperands > 1 ? parse_operand(static_cast<std::uint64_t>(stmt.operands[1]),
                                                 ctx, locals, unusedTag)
                                 : 0;
        return static_cast<std::int64_t>(top.value) >= lower;
    }
    case core_op::kTryForParties:
        return top.value >= 0 &&
               static_cast<std::size_t>(top.value) < ctx.world.parties.size();
    case core_op::kTryForAgents:
        return top.value >= 0 &&
               static_cast<std::size_t>(top.value) < ctx.world.agents.size();
    case core_op::kTryForPropInstances:
        return top.value >= 0 &&
               static_cast<std::size_t>(top.value) < ctx.world.mission_objects.size();
    case core_op::kTryForPlayers:
        return top.value >= 0 && top.value < ctx.world.max_players &&
               static_cast<std::size_t>(top.value) < ctx.world.players.size();
    case core_op::kTryForDictKeys:
        return top.value != -1;
    default:
        return false;
    }
}

inline void start_loop(const StatementBlock& block,
                       VmContext& ctx,
                       std::span<std::int64_t> locals,
                       std::span<LoopFrame> loops,
                       std::int32_t& numLoops,
                       std::int32_t& pc) {
    const Statement& stmt = block.statements[static_cast<std::size_t>(pc)];
    const std::uint32_t opcode = stmt.opcode & kOpcodeMask;
    std::uint8_t unusedTag = 0;

    LoopFrame frame{pc, 0};
    bool isDictLoop = false;
    if (opcode == core_op::kTryForRange) {
        frame.value = stmt.numOperands > 1
                          ? static_cast<std::int32_t>(parse_operand(
                                static_cast<std::uint64_t>(stmt.operands[1]), ctx, locals, unusedTag))
                          : 0;
    } else if (opcode == core_op::kTryForRangeBackwards) {
        const std::int64_t upper =
            stmt.numOperands > 2
                ? parse_operand(static_cast<std::uint64_t>(stmt.operands[2]), ctx, locals, unusedTag)
                : 0;
        frame.value = static_cast<std::int32_t>(upper - 1);
    } else if (opcode == core_op::kTryForParties) {
        frame.value = find_next_valid_party(ctx, 0);
    } else if (opcode == core_op::kTryForAgents) {
        frame.value = find_next_valid_agent(stmt, ctx, locals, 0, true);
    } else if (opcode == core_op::kTryForPropInstances) {
        frame.value = find_next_valid_prop(stmt, ctx, locals, 0);
    } else if (opcode == core_op::kTryForPlayers) {
        const std::int32_t startIdx =
            (stmt.numOperands > 1 &&
             parse_operand(static_cast<std::uint64_t>(stmt.operands[1]), ctx, locals, unusedTag) != 0)
                ? 1
                : 0;
        frame.value = find_next_valid_player(ctx, startIdx);
    } else if (opcode == core_op::kTryForDictKeys) {
        isDictLoop = true;
        frame.value = -1;
        const std::int32_t dictId =
            stmt.numOperands > 1
                ? static_cast<std::int32_t>(parse_operand(
                      static_cast<std::uint64_t>(stmt.operands[1]), ctx, locals, unusedTag))
                : -1;
        const WseDictionary* dict = ctx.dictionaries.get(dictId);
        if (dict != nullptr && !dict->empty()) {
            frame.dictId = dictId;
            frame.dictKey = dict->entries.begin()->first;
            frame.value = 1;
        }
    }

    if (static_cast<std::size_t>(numLoops) < loops.size()) {
        loops[static_cast<std::size_t>(numLoops)] = frame;
        ++numLoops;
    } else {
        ctx.report_error("Loop stack overflow in start_loop");
        pc = stmt.endStatement;
        return;
    }

    if (!isDictLoop && stmt.numOperands > 0) {
        store_lvalue(static_cast<std::uint64_t>(stmt.operands[0]),
                     static_cast<std::int64_t>(frame.value), ctx, locals);
    }

    if (can_loop(block, ctx, locals, loops, numLoops)) {
        if (isDictLoop) {
            set_dict_loop_key(stmt, frame, ctx, locals);
        }
        ++pc;
    } else {
        // Retail 0x1401f7a2c jumps directly to try_end, which executes end_loop
        // on the next iteration and pops the unentered loop frame.
        pc = stmt.endStatement;
    }
}

inline void end_loop(const StatementBlock& block,
                     VmContext& ctx,
                     std::span<std::int64_t> locals,
                     std::span<LoopFrame> loops,
                     std::int32_t& numLoops,
                     std::int32_t& pc) {
    ++pc;
    if (numLoops <= 0) {
        return;
    }

    LoopFrame& top = loops[static_cast<std::size_t>(numLoops - 1)];
    const Statement& startStmt = block.statements[static_cast<std::size_t>(top.statementNo)];
    const std::uint32_t opcode = startStmt.opcode & kOpcodeMask;

    if (opcode == core_op::kTryBegin) {
        --numLoops;
        return;
    }

    bool isDictLoop = false;
    if (opcode == core_op::kTryForRange) {
        ++top.value;
    } else if (opcode == core_op::kTryForRangeBackwards) {
        --top.value;
    } else if (opcode == core_op::kTryForParties) {
        top.value = find_next_valid_party(ctx, top.value + 1);
    } else if (opcode == core_op::kTryForAgents) {
        top.value = find_next_valid_agent(startStmt, ctx, locals, top.value + 1, false);
    } else if (opcode == core_op::kTryForPropInstances) {
        top.value = find_next_valid_prop(startStmt, ctx, locals, top.value + 1);
    } else if (opcode == core_op::kTryForPlayers) {
        top.value = find_next_valid_player(ctx, top.value + 1);
    } else if (opcode == core_op::kTryForDictKeys) {
        isDictLoop = true;
        // Re-resolve the dictionary and step to the first key after the one
        // just visited; a freed, cleared, or shrunk dictionary ends the loop.
        const WseDictionary* dict =
            top.value != -1 ? ctx.dictionaries.get(top.dictId) : nullptr;
        top.value = -1;
        if (dict != nullptr) {
            const auto next = dict->entries.upper_bound(top.dictKey);
            if (next != dict->entries.end()) {
                top.dictKey = next->first;
                top.value = 1;
            }
        }
    } else {
        --numLoops;
        return;
    }

    if (can_loop(block, ctx, locals, loops, numLoops)) {
        if (isDictLoop) {
            set_dict_loop_key(startStmt, top, ctx, locals);
        } else if (startStmt.numOperands > 0) {
            store_lvalue(static_cast<std::uint64_t>(startStmt.operands[0]),
                         static_cast<std::int64_t>(top.value), ctx, locals);
        }
        pc = top.statementNo + 1;
    } else {
        --numLoops;
    }
}

inline bool execute_statement_block(StatementBlock& block,
                                    std::int32_t caller,
                                    std::int32_t depth,
                                    std::span<const std::int64_t> params,
                                    VmContext& ctx);

inline bool execute_call_script(const Statement& stmt,
                                std::int32_t caller,
                                std::int32_t depth,
                                VmContext& ctx,
                                std::span<const std::int64_t> locals) {
    std::uint8_t unusedTag = 0;
    const std::int32_t scriptIdx =
        stmt.numOperands > 0
            ? static_cast<std::int32_t>(parse_operand(
                  static_cast<std::uint64_t>(stmt.operands[0]), ctx, locals, unusedTag))
            : -1;

    if (ctx.scripts == nullptr || scriptIdx < 0 ||
        static_cast<std::size_t>(scriptIdx) >= ctx.scripts->scripts.size() ||
        depth > ctx.maxCallDepth) {
        if (depth > ctx.maxCallDepth) {
            ctx.report_error("Script call stack depth limit exceeded");
        } else {
            ctx.report_error("Invalid script index in call_script");
        }
        return false;
    }

    std::array<std::int64_t, kMaxOperands> childParams{};
    const std::int32_t count = std::max<std::int32_t>(0, stmt.numOperands - 1);
    const auto clampedCount =
        static_cast<std::size_t>(std::min<std::int32_t>(count, static_cast<std::int32_t>(kMaxOperands)));
    for (std::size_t i = 0; i < clampedCount; ++i) {
        childParams[i] = parse_operand(static_cast<std::uint64_t>(stmt.operands[i + 1]), ctx,
                                       locals, unusedTag);
    }

    StatementBlock& targetBlock = ctx.scripts->scripts[static_cast<std::size_t>(scriptIdx)].block;
    return execute_statement_block(
        targetBlock, caller, depth + 1, std::span<const std::int64_t>(childParams.data(), clampedCount),
        ctx);
}

// Interprets `block` (0x1401f6b50). Analyzes lazily on first run, tracks call
// depth and loop frames, and returns true unless a top-level (numLoops == 0)
// statement or nested call_script fails.
inline bool execute_statement_block(StatementBlock& block,
                                    std::int32_t caller,
                                    std::int32_t depth,
                                    std::span<const std::int64_t> params,
                                    VmContext& ctx) {
    if (depth < 0 || depth > ctx.maxCallDepth + 1) {
        ctx.report_error("Statement block depth out of range");
        return false;
    }

    struct DepthGuard {
        std::int32_t& slot;
        std::int32_t& active;
        std::int32_t previous;
        explicit DepthGuard(std::int32_t& target, std::int32_t& blocks, std::int32_t next) noexcept
            : slot(target), active(blocks), previous(target) {
            slot = next;
            ++active;
        }
        ~DepthGuard() noexcept {
            slot = previous;
            --active;
        }
    };
    const DepthGuard depthGuard(ctx.currentDepth, ctx.activeBlocks, depth);

    std::vector<std::int64_t> hookedParamsStorage;
    if (block.luaHookRef != kNoLuaHook && ctx.luaRuntime != nullptr && ctx.luaRuntime->is_ready()) {
        auto hookRes = ctx.luaRuntime->on_script_execute(block.luaHookRef, params, ctx);
        if (!hookRes.execute) {
            return hookRes.cf_result;
        }
        if (hookRes.replace_params) {
            hookedParamsStorage = std::move(hookRes.replacement_params);
            params = std::span<const std::int64_t>(hookedParamsStorage);
        }
    }

    analyze_statement_block(block, &ctx);
    if (depth > ctx.maxObservedDepth) {
        ctx.maxObservedDepth = depth;
    }

    const auto depthIdx = static_cast<std::size_t>(depth);
    // Callers further up hold `locals` spans into these vectors across nested calls, so the
    // storage must never move: reserve every depth the entry check above allows on first growth.
    if (ctx.localFrames.size() <= depthIdx || ctx.loopFrames.size() <= depthIdx) {
        const auto capacity = static_cast<std::size_t>(std::max(ctx.maxCallDepth, 0)) + 2U;
        ctx.localFrames.reserve(std::max(capacity, depthIdx + 1));
        ctx.loopFrames.reserve(std::max(capacity, depthIdx + 1));
        if (ctx.localFrames.size() <= depthIdx) {
            ctx.localFrames.resize(depthIdx + 1);
        }
        if (ctx.loopFrames.size() <= depthIdx) {
            ctx.loopFrames.resize(depthIdx + 1);
        }
    }

    if (ctx.clearLocalsBetweenCalls) {
        const auto zeroCount = static_cast<std::size_t>(
            std::clamp(block.numLocalVariablesUsed, static_cast<std::int32_t>(kMinLocalVariables),
                       kDefaultLocalVariables));
        std::fill_n(ctx.localFrames[depthIdx].begin(), zeroCount, 0LL);
    }

    ctx.skipStatement = false;
    bool blockResult = true;
    bool canEnterElse = true;
    std::int32_t numLoops = 0;
    std::int32_t pc = 0;
    const auto numStatements = static_cast<std::int32_t>(block.statements.size());

    while (pc < numStatements) {
        const std::span<std::int64_t> locals(ctx.localFrames[depthIdx]);
        const std::span<LoopFrame> loops(ctx.loopFrames[depthIdx]);
        const Statement& stmt = block.statements[static_cast<std::size_t>(pc)];
        const std::uint32_t maskedOp = stmt.opcode & kOpcodeMask;
        bool currentOk = true;

        if (maskedOp >= kNativeOpcodeMin) {
            const std::size_t unimplementedBefore = ctx.unimplementedCount;
            if (execute_statement(stmt, caller, ctx, locals)) {
                ++pc;
            } else {
                if (ctx.failOnUnimplemented && ctx.unimplementedCount > unimplementedBefore) {
                    return false;
                }
                pc = stmt.endStatement;
                canEnterElse = false;
                currentOk = false;
            }
        } else if (maskedOp == core_op::kCallScript) {
            const std::size_t unimplementedBefore = ctx.unimplementedCount;
            if (execute_call_script(stmt, caller, depth, ctx, locals)) {
                ++pc;
            } else {
                if (ctx.failOnUnimplemented && ctx.unimplementedCount > unimplementedBefore) {
                    return false;
                }
                pc = stmt.endStatement;
                canEnterElse = false;
                currentOk = false;
            }
        } else if (maskedOp == core_op::kStoreScriptParam1) {
            if (params.empty()) {
                ctx.report_error("Invalid Script Parameter ID: 1");
            } else if (stmt.numOperands > 0) {
                store_lvalue(static_cast<std::uint64_t>(stmt.operands[0]), params[0], ctx, locals);
            }
            ++pc;
        } else if (maskedOp == core_op::kStoreScriptParam2) {
            if (params.size() < 2U) {
                ctx.report_error("Invalid Script Parameter ID: 2");
            } else if (stmt.numOperands > 0) {
                store_lvalue(static_cast<std::uint64_t>(stmt.operands[0]), params[1], ctx, locals);
            }
            ++pc;
        } else if (maskedOp == core_op::kStoreScriptParam) {
            std::uint8_t unusedTag = 0;
            const std::int32_t oneBased =
                stmt.numOperands > 1
                    ? static_cast<std::int32_t>(parse_operand(
                          static_cast<std::uint64_t>(stmt.operands[1]), ctx, locals, unusedTag))
                    : 0;
            const std::int32_t zeroBased = oneBased - 1;
            if (zeroBased < 0 || static_cast<std::size_t>(zeroBased) >= params.size()) {
                ctx.report_error("Invalid Script Parameter ID");
            } else if (stmt.numOperands > 0) {
                store_lvalue(static_cast<std::uint64_t>(stmt.operands[0]),
                             params[static_cast<std::size_t>(zeroBased)], ctx, locals);
            }
            ++pc;
        } else if (maskedOp == core_op::kBreakLoop) {
            std::int32_t i = numLoops - 1;
            bool found = false;
            while (i >= 0 && !found) {
                numLoops = i;
                const Statement& target =
                    block.statements[static_cast<std::size_t>(loops[static_cast<std::size_t>(i)].statementNo)];
                if ((target.opcode & kOpcodeMask) != core_op::kTryBegin) {
                    pc = target.endStatement;
                    found = true;
                }
                --i;
            }
            ++pc;
        } else if (maskedOp == core_op::kContinueLoop) {
            std::int32_t i = numLoops - 1;
            bool found = false;
            while (i >= 0 && !found) {
                const Statement& target =
                    block.statements[static_cast<std::size_t>(loops[static_cast<std::size_t>(i)].statementNo)];
                if ((target.opcode & kOpcodeMask) != core_op::kTryBegin) {
                    pc = target.endStatement;
                    numLoops = i + 1;
                    found = true;
                }
                --i;
            }
            if (!found) {
                ++pc;
            }
        } else if (maskedOp == core_op::kTryEnd) {
            canEnterElse = true;
            end_loop(block, ctx, locals, loops, numLoops, pc);
        } else if (maskedOp == core_op::kTryBegin) {
            canEnterElse = true;
            if (static_cast<std::size_t>(numLoops) < loops.size()) {
                loops[static_cast<std::size_t>(numLoops)] = LoopFrame{pc, 0};
                ++numLoops;
            } else {
                ctx.report_error("Loop stack overflow on try_begin");
            }
            ++pc;
        } else if (maskedOp == core_op::kElseTry) {
            if (!canEnterElse) {
                canEnterElse = true;
                ++pc;
            } else {
                pc = stmt.endStatement;
            }
        } else if (is_block_opener(maskedOp)) {
            canEnterElse = true;
            start_loop(block, ctx, locals, loops, numLoops, pc);
        } else {
            ctx.record_unimplemented(maskedOp);
            if (ctx.failOnUnimplemented) {
                return false;
            }
            ++pc;
        }

        if (numLoops == 0) {
            blockResult = currentOk;
        }
        if (!blockResult) {
            return false;
        }
    }

    return blockResult;
}

inline bool execute_statement_block(StatementBlock& block, VmContext& ctx) {
    return execute_statement_block(block, 0, 0, {}, ctx);
}

[[nodiscard]] inline bool wse_invoke_script_callback(
    void* user,
    std::int32_t script_index,
    std::span<const std::int64_t> params) {
    auto* ctx = static_cast<VmContext*>(user);
    if (ctx == nullptr || ctx->scripts == nullptr || script_index < 0 ||
        static_cast<std::size_t>(script_index) >= ctx->scripts->scripts.size()) {
        return false;
    }
    const bool prev_skip = ctx->skipStatement;
    const bool ok = execute_statement_block(
        ctx->scripts->scripts[static_cast<std::size_t>(script_index)].block,
        kScriptCaller, ctx->currentDepth + 1, params, *ctx);
    ctx->skipStatement = prev_skip;
    return ok;
}

// Executes a loaded script by index (0x1400dbf40, default caller 0x200000).
inline bool execute_script(game::ScriptManager& manager,
                           std::int32_t scriptIndex,
                           VmContext& ctx,
                           std::span<const std::int64_t> params = {},
                           std::int32_t caller = kScriptCaller) {
    if (scriptIndex < 0 || static_cast<std::size_t>(scriptIndex) >= manager.scripts.size()) {
        return false;
    }
    game::ScriptManager* previousScripts = ctx.scripts;
    if (ctx.scripts == nullptr) {
        ctx.scripts = &manager;
    }
    // Called from inside a running block (a native handler running a mapped script): use the
    // next frame and keep the caller's this_or_next state, as `wse_invoke_script_callback` does.
    const bool nested = ctx.activeBlocks > 0;
    const bool previousSkip = ctx.skipStatement;
    const bool result = execute_statement_block(
        manager.scripts[static_cast<std::size_t>(scriptIndex)].block, caller,
        nested ? ctx.currentDepth + 1 : 0, params, ctx);
    if (nested) {
        ctx.skipStatement = previousSkip;
    }
    ctx.scripts = previousScripts;
    return result;
}

inline bool execute_script(game::ScriptManager& manager,
                           std::string_view scriptId,
                           VmContext& ctx,
                           std::span<const std::int64_t> params = {},
                           std::int32_t caller = kScriptCaller) {
    for (std::size_t i = 0; i < manager.scripts.size(); ++i) {
        if (manager.scripts[i].id == scriptId) {
            return execute_script(manager, static_cast<std::int32_t>(i), ctx, params, caller);
        }
    }
    return false;
}

// Executes a mapped static script slot (0x1400dbfd0).
inline bool execute_mapped_script(game::ScriptManager& manager,
                                  std::size_t mappedSlot,
                                  VmContext& ctx,
                                  std::span<const std::int64_t> params = {},
                                  std::int32_t caller = kScriptCaller) {
    if (mappedSlot >= manager.mappedScripts.size()) {
        return false;
    }
    return execute_script(manager, manager.mappedScripts[mappedSlot], ctx, params, caller);
}

// Mapped script slots used below (`game::kMappedScriptNames`).
inline constexpr std::size_t kMappedGetPartyCompanionLimit = 0x15U;
inline constexpr std::size_t kMappedGetPartyPrisonerLimit = 0x16U;
inline constexpr std::size_t kMappedGetSkillModifierForTroop = 0x1bU;

// `mbGame::getTroopSkill(troop, skill, true)` (`0x1400dcd50`): the troop's level capped by the
// skill's maximum, plus whatever `game_get_skill_modifier_for_troop` (troop, skill) leaves in
// the trigger result (reset to 0 first), clamped to `[0, max level]`.
[[nodiscard]] inline std::int32_t troop_skill_level(VmContext& ctx,
                                                    std::int32_t troop_no,
                                                    std::int32_t skill_no) {
    game::WseWorldState& world = ctx.world;
    if (!world.is_valid_troop(troop_no) || skill_no < 0 ||
        static_cast<std::size_t>(skill_no) >= world.skills.size()) {
        return 0;
    }
    const std::int32_t cap =
        std::max<std::int32_t>(0, world.skills[static_cast<std::size_t>(skill_no)].max_level);
    std::int64_t level = game::troop_skill_capped(world, troop_no, skill_no);
    if (ctx.scripts != nullptr) {
        world.trigger_result = 0;
        const std::array<std::int64_t, 2> params{troop_no, skill_no};
        (void)execute_mapped_script(*ctx.scripts, kMappedGetSkillModifierForTroop, ctx, params);
        level += world.trigger_result;
    }
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(level, 0, cap));
}

// `mbParty::getMaxNumCompanions` (`0x1401c18a0`) / `getMaxNumPrisoners` (`0x1401c1920`): the
// module's `game_get_party_companion_limit` / `game_get_party_prisoner_limit` (party, 0) wins
// when it exists (its trigger result is the limit); otherwise the built-in formula with the
// leader's skill from `getTroopSkill(..., true)`.
[[nodiscard]] inline std::int32_t party_max_companions(VmContext& ctx, std::int32_t party_no) {
    game::WseWorldState& world = ctx.world;
    if (!world.is_valid_party(party_no)) {
        return 0;
    }
    if (ctx.scripts != nullptr) {
        const std::array<std::int64_t, 2> params{party_no, 0};
        if (execute_mapped_script(*ctx.scripts, kMappedGetPartyCompanionLimit, ctx, params)) {
            return static_cast<std::int32_t>(std::clamp<std::int64_t>(
                world.trigger_result, std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max()));
        }
    }
    const game::WseParty& party = world.parties[static_cast<std::size_t>(party_no)];
    const std::int32_t leader = game::party_leader_troop(world, party);
    return game::party_max_companions(world, party,
                                      leader >= 0 ? troop_skill_level(ctx, leader, 1) : -1);
}

[[nodiscard]] inline std::int32_t party_max_prisoners(VmContext& ctx, std::int32_t party_no) {
    game::WseWorldState& world = ctx.world;
    if (!world.is_valid_party(party_no)) {
        return 0;
    }
    if (ctx.scripts != nullptr) {
        const std::array<std::int64_t, 2> params{party_no, 0};
        if (execute_mapped_script(*ctx.scripts, kMappedGetPartyPrisonerLimit, ctx, params)) {
            return static_cast<std::int32_t>(std::clamp<std::int64_t>(
                world.trigger_result, std::numeric_limits<std::int32_t>::min(),
                std::numeric_limits<std::int32_t>::max()));
        }
    }
    const game::WseParty& party = world.parties[static_cast<std::size_t>(party_no)];
    const std::int32_t leader = game::party_leader_troop(world, party);
    return game::party_max_prisoners(world, party,
                                     leader >= 0 ? troop_skill_level(ctx, leader, 2) : -1);
}

// Marks a trigger (or trigger-manager) execution in progress. Lua callbacks
// reached from inside it must not resize the trigger vectors being walked, so
// the Lua runtime queues those edits and this scope applies them once the
// outermost trigger execution has unwound. Safety deviation: retail edits the
// arrays in place.
//
// The edits run when the outermost scope ends, and they may resize
// ModuleBundle::presentations and the per-template / per-item / per-prop
// trigger vectors. A caller that walks one of those containers and runs a
// trigger manager per element must therefore hold its own TriggerIterationScope
// around the whole walk, so the edits wait until the walk is finished.
class TriggerIterationScope {
public:
    explicit TriggerIterationScope(VmContext& ctx) noexcept : ctx_(ctx) {
        ++ctx_.triggerIterationDepth;
    }
    ~TriggerIterationScope() {
        if (--ctx_.triggerIterationDepth == 0 && ctx_.luaRuntime != nullptr) {
            // A destructor must not throw (it may run during unwinding); an
            // edit that fails to allocate is dropped rather than terminating.
            try {
                ctx_.luaRuntime->apply_deferred_edits();
            } catch (...) {
                try {
                    ctx_.report_error("Lua deferred trigger edit failed");
                } catch (...) {
                }
            }
        }
    }
    TriggerIterationScope(const TriggerIterationScope&) = delete;
    TriggerIterationScope& operator=(const TriggerIterationScope&) = delete;

private:
    VmContext& ctx_;
};

// Drives one trigger state machine against VmContext, passing eventKey as caller
// so WSE ops like store_current_trigger (3014) observe the active trigger event.
[[nodiscard]] inline bool execute_trigger(game::Trigger& trigger,
                                          std::int32_t eventKey,
                                          std::span<const std::int64_t> timersTotal,
                                          VmContext& ctx) {
    const TriggerIterationScope iterationScope(ctx);
    std::int64_t now = 0;
    auto timer_elapsed = [&](const game::AppTimer& t, double interval,
                             bool& out_elapsed) -> bool {
        if (!game::timer_now(timersTotal, t, now)) return false;
        out_elapsed = interval <= game::timer_elapsed(now, t.value);
        return true;
    };
    auto reset_timer = [&](game::AppTimer& t) -> bool {
        if (!game::timer_now(timersTotal, t, now)) return false;
        t.value = now;
        return true;
    };

    if (trigger.status == 2) {
        bool elapsed = false;
        if (!timer_elapsed(trigger.rearmTimer, trigger.rearmInterval, elapsed)) return false;
        if (elapsed) {
            trigger.status = 0;
            if (!reset_timer(trigger.checkTimer)) return false;
        }
    }

    if (trigger.status == 0) {
        if (trigger.checkInterval <= game::kManagerEventThreshold) {
            if (game::interval_key(trigger.checkInterval) == eventKey &&
                execute_statement_block(trigger.conditions, eventKey, 0, {}, ctx)) {
                trigger.status = 2;
                if (!reset_timer(trigger.rearmTimer)) return false;
                (void)execute_statement_block(trigger.consequences, eventKey, 0, {}, ctx);
            }
        } else if (eventKey >= 0) {
            bool elapsed = false;
            if (!timer_elapsed(trigger.checkTimer, trigger.checkInterval, elapsed)) return false;
            if (elapsed) {
                trigger.checkTimer.value = now;
                if (execute_statement_block(trigger.conditions, eventKey, 0, {}, ctx)) {
                    trigger.status = 1;
                    if (!reset_timer(trigger.delayTimer)) return false;
                }
            }
        }
    }

    if (trigger.status == 1) {
        bool elapsed = false;
        if (!timer_elapsed(trigger.delayTimer, trigger.delayInterval, elapsed)) return false;
        if (elapsed) {
            trigger.status = 2;
            if (!reset_timer(trigger.rearmTimer)) return false;
            (void)execute_statement_block(trigger.consequences, eventKey, 0, {}, ctx);
        }
    }

    return true;
}

[[nodiscard]] inline bool execute_trigger_manager(game::TriggerManager& manager,
                                                  std::int32_t eventKey,
                                                  std::span<const std::int64_t> timersTotal,
                                                  VmContext& ctx) {
    const TriggerIterationScope iterationScope(ctx);
    for (auto& trigger : manager.triggers) {
        if (!execute_trigger(trigger, eventKey, timersTotal, ctx)) return false;
    }
    return true;
}

[[nodiscard]] inline bool execute_simple_trigger(game::SimpleTrigger& trigger,
                                                 std::int32_t eventKey,
                                                 std::span<const std::int64_t> timersTotal,
                                                 VmContext& ctx) {
    const TriggerIterationScope iterationScope(ctx);
    if (trigger.interval < game::kSimpleEventThreshold) {
        if (game::interval_key(trigger.interval) != eventKey) return true;
    } else {
        if (eventKey < 0) return true;
        std::int64_t now = 0;
        if (!game::timer_now(timersTotal, trigger.timer, now)) return false;
        if (game::timer_elapsed(now, trigger.timer.value) < trigger.interval) return true;
        trigger.timer.value = now;
    }

    (void)execute_statement_block(trigger.block, eventKey, 0, {}, ctx);
    return true;
}

[[nodiscard]] inline bool execute_simple_trigger_manager(game::SimpleTriggerManager& manager,
                                                         std::int32_t eventKey,
                                                         std::span<const std::int64_t> timersTotal,
                                                         VmContext& ctx) {
    const TriggerIterationScope iterationScope(ctx);
    for (auto& trigger : manager.triggers) {
        if (!execute_simple_trigger(trigger, eventKey, timersTotal, ctx)) return false;
        // Mirrors mbSimpleTriggerManager::execute (0x140210352..0x140210377):
        // positive odd event keys abort the remaining triggers once a type-0x12 window (`mbMenuWindow`,
        // enqueued by `jump_to_menu` `0x1401dc431`) sits at the back of `enqueued_windows`.
        // Campaign frames consume and pop it via `WseWorldState::consume_campaign_queued_window`
        // (`mbMapWindow::frameMove` `0x1401180f0` + `mbMenuWindow::open` `0x1401203a0`).
        if (eventKey > 0 && (eventKey & 1) != 0 && !ctx.world.m5.enqueued_windows.empty() &&
            ctx.world.m5.enqueued_windows.back().first == game::kQueuedWindowMenuType) {
            break;
        }
    }
    return true;
}

}  // namespace wse::vm

namespace wse::game {

using vm::execute_mapped_script;
using vm::execute_script;
using vm::execute_simple_trigger;
using vm::execute_simple_trigger_manager;
using vm::execute_trigger;
using vm::execute_trigger_manager;

}  // namespace wse::game

#include "wse/vm/native_ops.h"
