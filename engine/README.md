# Portable engine sources

Clean-room C++20 reconstruction of the WSE2 engine. Code here is written from the evidence in
`docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json`; it contains no lifted machine code.

## Current modules

| File | Reconstructs | Evidence |
| --- | --- | --- |
| `include/wse/io/text_stream.h` | whitespace token reader and typed decimal parsers | `rglInputTextFileStream` vftable `0x14051c0b0`, `rglTextStream` vftable `0x14051c2b0`, `mbGame::readTriggers` `0x1400cca30` |
| `include/wse/vm/operand.h` | tagged operand decode and lvalue write-back | `mbStatement::parseOperand` `0x1401c2100`, `mbStatement::returnValue` `0x1401c2190`, `mbStatementBlock::execute` `0x1401f2530`, `analyze` `0x1401f22a0` |
| `include/wse/vm/slots.h` | `mbSlots` (`std::vector<int64_t>`) get/set semantics | PDB type `0x1b004`; `parseOperand` bounds check; `returnValue` write path |
| `include/wse/vm/statement.h` | statement record and text reader | `mbStatementBlock::readStream` `0x1401f2160`, PDB type `0xb82a` |
| `include/wse/game/trigger.h` | trigger/simple-trigger records and loaders | `0x14023ea20`, `0x14023e750`, `0x14020b9d0`, `0x14020b620`, `0x1400cca30`, `0x1400ccde0` |
| `tests/operand_test.cpp` | host tests for tags, decode, store, slots, tokenizer, statements | local test doubles only |
| `tests/trigger_test.cpp` | host tests for trigger and manager readers | local test doubles only |

## Module text format

Module files are whitespace-delimited decimal text. The retail header is three tokens
(`triggersfile version 1`, `simple_triggersfile version 1`, `scriptsfile version 1`, …); the readers
skip bytes `<= 0x20`, take the next token, and parse it. There are no binary records.

Operands are 64-bit words. Values `<= 0` are immediates; otherwise bits 56–62 hold the tag and
bits 0–55 the payload.

| Tag | Meaning |
| ---: | --- |
| `0x00` | immediate |
| `0x01` | global variable array (unchecked in the original) |
| `0x02` | `mbSlots` vector (missing slot reads as 0) |
| `0x11` | local variable (unchecked in the original) |

A statement is `<opcode> <operandCount> <operand>...`. `m_endStatement` is initialized to `-1` by
the constructor and filled by `analyze`.

A trigger is three floats, the conditions block, and the consequences block. A simple trigger is
one float followed by its statement block. Manager records seed every `rglAppTimer` from
`m_defaultTimerNo` and `g_timersTotal[m_defaultTimerNo]`.

## Safety deviations

The retail code trusts operand/statement/trigger counts and timer indices. This port rejects
negative counts, operands above 32, oversized arrays, and out-of-range timer indices; none of these
checks changes valid module behaviour.

## Build and test

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1
```

Expected tail:

```
operand tests: PASS
trigger tests: PASS
engine host tests: PASS
```

The Android half of the gate (`clang++ -target aarch64-linux-android29 -Wall -Werror`) cannot run
until an NDK is installed; see `STATUS.md`.
