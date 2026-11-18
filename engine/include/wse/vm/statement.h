// Decoded script-VM statement record and module-file reader.
//
// Evidence:
//   mbStatementBlock::readStream (0x1401f6780)  Ghidra 12.1.4 pseudocode
//   mbStatement layout            PDB TPI type 0xb91e, size 272
//   rglTextStream reader slots    rglInputTextFileStream vtable 0x140566030
//
// Module text form per statement:
//   <opcode> <operandCount> <operand>...
// all whitespace-delimited decimal tokens. The operand count is read with the
// int32 slot (+0x80), the opcode with the uint32 slot (+0x78), and every
// operand with the int64 slot (+0x70).
//
// In memory the record stays 272 bytes: uint32 opcode, int32 operandCount,
// 32 int64 operands, int32 endStatement initialized to -1 by the constructor
// (0x1401c61e0) and filled by analyze() (0x1401f68c0).
//
// Safety deviation: the original passes the count straight to the allocator
// and reads that many operands into the fixed 32-element array. This reader
// rejects negative counts, counts above 32, and statement arrays above
// kMaxStatements, and allocates statements as they are read rather than
// trusting the count.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wse/io/text_stream.h"
#include "wse/vm/operand.h"

namespace wse::vm {

inline constexpr std::size_t kStatementSize = 0x110;
inline constexpr std::size_t kMaxStatements = 1u << 20;
inline constexpr std::int32_t kDefaultLocalVariables = 0x400;
inline constexpr std::int32_t kNoLuaHook = -2;

struct Statement {
    std::uint32_t opcode = 0;
    std::int32_t numOperands = 0;
    std::int64_t operands[kMaxOperands]{};
    std::int32_t endStatement = -1;
};

struct StatementBlock {
    std::vector<Statement> statements;
    bool analyzed = false;
    std::int32_t numLocalVariablesUsed = kDefaultLocalVariables;
    std::int32_t luaHookRef = kNoLuaHook;
    std::string id;
};

enum class ReadError : std::uint8_t {
    none = 0,
    malformed_number,
    malformed_string,
    negative_count,
    operand_count_out_of_range,
    statement_count_out_of_range,
    trigger_count_out_of_range,
    script_count_out_of_range,
    timer_index_out_of_range,
};

[[nodiscard]] constexpr std::string_view describe(ReadError error) noexcept {
    switch (error) {
    case ReadError::none:
        return "ok";
    case ReadError::malformed_number:
        return "missing or malformed number token";
    case ReadError::malformed_string:
        return "missing or malformed string token";
    case ReadError::negative_count:
        return "negative record count";
    case ReadError::operand_count_out_of_range:
        return "operand count exceeds 32";
    case ReadError::statement_count_out_of_range:
        return "statement count out of range";
    case ReadError::trigger_count_out_of_range:
        return "trigger count out of range";
    case ReadError::script_count_out_of_range:
        return "script count out of range";
    case ReadError::timer_index_out_of_range:
        return "timer index out of range";
    }
    return "unknown error";
}

// Reads one `<opcode> <count> <operand>...` record. On failure `out` may be
// partially updated.
[[nodiscard]] inline ReadError read_statement(io::TokenReader& reader, Statement& out) {
    std::uint32_t opcode = 0;
    if (!reader.read_u32(opcode)) {
        return ReadError::malformed_number;
    }

    std::int32_t count = 0;
    if (!reader.read_i32(count)) {
        return ReadError::malformed_number;
    }
    if (count < 0) {
        return ReadError::negative_count;
    }
    if (static_cast<std::uint32_t>(count) > kMaxOperands) {
        return ReadError::operand_count_out_of_range;
    }

    out.opcode = opcode;
    out.numOperands = count;
    for (std::int32_t index = 0; index < count; ++index) {
        if (!reader.read_i64(out.operands[index])) {
            return ReadError::malformed_number;
        }
    }
    out.endStatement = -1;
    return ReadError::none;
}

// Reads the leading statement count and then that many statements.
[[nodiscard]] inline ReadError read_statement_block(io::TokenReader& reader,
                                                    StatementBlock& block) {
    block.analyzed = false;
    block.numLocalVariablesUsed = kDefaultLocalVariables;

    std::int32_t count = 0;
    if (!reader.read_i32(count)) {
        return ReadError::malformed_number;
    }
    if (count < 0) {
        return ReadError::negative_count;
    }
    if (static_cast<std::uint64_t>(count) > kMaxStatements) {
        return ReadError::statement_count_out_of_range;
    }

    // Grow as records are read. The shortest statement is "0 0" plus a
    // separator, so reserving only what the unread input can hold keeps a
    // corrupt count from allocating kMaxStatements * 272 bytes up front.
    const auto total = static_cast<std::size_t>(count);
    block.statements.clear();
    block.statements.reserve(reader.plausible_count(total, 4));
    for (std::size_t index = 0; index < total; ++index) {
        Statement& statement = block.statements.emplace_back();
        const ReadError error = read_statement(reader, statement);
        if (error != ReadError::none) {
            return error;
        }
    }
    return ReadError::none;
}

}  // namespace wse::vm
