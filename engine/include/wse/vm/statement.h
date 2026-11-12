// Decoded script-VM statement record and binary reader.
//
// Evidence:
//   mbStatementBlock::readStream (0x1401f2160)  Ghidra 12.1.4 pseudocode
//   mbStatement layout            PDB TPI type 0xb82a, size 272
//
// Binary form per statement:
//   uint32  opcode
//   int32   operandCount
//   int64   operands[operandCount]
// and m_endStatement is initialized to -1 before analyze() computes jumps.
//
// Safety deviation: the original passes an unchecked operand count to the
// allocator and later reads that many int64 values into the fixed 32-element
// array. This reader rejects counts outside [0, 32] instead.

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "wse/vm/operand.h"

namespace wse::vm {

inline constexpr std::size_t kStatementSize = 0x110;

struct Statement {
    std::uint32_t opcode;
    std::int32_t numOperands;
    std::int64_t operands[kMaxOperands];
    std::int32_t endStatement;
};

static_assert(std::endian::native == std::endian::little,
              "the module binary format is little-endian");
static_assert(offsetof(Statement, opcode) == 0x000);
static_assert(offsetof(Statement, numOperands) == 0x004);
static_assert(offsetof(Statement, operands) == 0x008);
static_assert(offsetof(Statement, endStatement) == 0x108);
static_assert(sizeof(Statement) == kStatementSize);

class ByteSource {
public:
    virtual ~ByteSource() = default;

    // Reads exactly `bytes` bytes or returns false.
    virtual bool read(void* destination, std::size_t bytes) = 0;
};

enum class ReadError : std::uint8_t {
    none = 0,
    io,
    negative_operand_count,
    operand_count_out_of_range,
};

[[nodiscard]] constexpr std::string_view describe(ReadError error) noexcept {
    switch (error) {
    case ReadError::none:
        return "ok";
    case ReadError::io:
        return "truncated statement";
    case ReadError::negative_operand_count:
        return "negative operand count";
    case ReadError::operand_count_out_of_range:
        return "operand count exceeds 32";
    }
    return "unknown error";
}

// Reads one statement. On failure `out` may be partially updated.
[[nodiscard]] inline ReadError read_statement(ByteSource& source,
                                              Statement& out) noexcept {
    std::uint32_t opcode = 0;
    std::int32_t count = 0;
    if (!source.read(&opcode, sizeof(opcode))) {
        return ReadError::io;
    }
    if (!source.read(&count, sizeof(count))) {
        return ReadError::io;
    }
    if (count < 0) {
        return ReadError::negative_operand_count;
    }
    if (static_cast<std::uint32_t>(count) > kMaxOperands) {
        return ReadError::operand_count_out_of_range;
    }

    out.opcode = opcode;
    out.numOperands = count;
    if (count > 0 &&
        !source.read(out.operands, static_cast<std::size_t>(count) * sizeof(std::int64_t))) {
        return ReadError::io;
    }
    out.endStatement = -1;
    return ReadError::none;
}

}  // namespace wse::vm
