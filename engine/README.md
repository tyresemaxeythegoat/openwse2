# Portable engine sources

Clean-room C++20 reconstruction of the WSE2 engine. Code here is written from the evidence in
`docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json`; it contains no lifted machine code.

## Current modules

| File | Reconstructs | Evidence |
| --- | --- | --- |
| `include/wse/vm/operand.h` | tagged operand decode and lvalue write-back | `mbStatement::parseOperand` `0x1401c2100`, `mbStatement::returnValue` `0x1401c2190`, `mbStatementBlock::execute` `0x1401f2530`, `analyze` `0x1401f22a0` |
| `include/wse/vm/statement.h` | 272-byte statement record and binary reader | `mbStatementBlock::readStream` `0x1401f2160`, PDB type `0xb82a` |
| `tests/operand_test.cpp` | host tests for tags, decode, store, and statement reads | local test double only |

## Format summary

Operands are 64-bit words. Values `<= 0` are immediates; otherwise bits 56–62 hold the tag and
bits 0–55 the payload.

| Tag | Meaning |
| ---: | --- |
| `0x00` | immediate |
| `0x01` | global variable array (unchecked in the original) |
| `0x02` | `mbSlots` vector (missing slot reads as 0) |
| `0x11` | local variable (unchecked in the original) |

A statement is `uint32 opcode`, `int32 operandCount`, then `operandCount` little-endian `int64`
operands. `m_endStatement` is initialized to `-1` by the constructor and filled by `analyze`.

## Safety deviations

The retail code trusts operand and statement counts. This port rejects an operand count above 32
and bounds-checks the global and local arrays; neither check changes valid module behaviour.

## Build and test

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1
```

Expected tail:

```
operand tests: PASS
engine host tests: PASS
```

The Android half of the gate (`clang++ -target aarch64-linux-android29 -Wall -Werror`) cannot run
until an NDK is installed; see `STATUS.md`.
