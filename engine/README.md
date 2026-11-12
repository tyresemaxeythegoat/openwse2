# Portable engine sources

Clean-room C++20 reconstruction of the WSE2 engine. Code here is written from the evidence in
`docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json`; it contains no lifted machine code.

## Current modules

| File | Reconstructs | Evidence |
| --- | --- | --- |
| `include/wse/io/text_stream.h` | whitespace token reader and typed decimal parsers | `rglInputTextFileStream` vftable `0x14051c0b0`, `rglTextStream` readers `0x1402a2ba0`–`0x1402a2f80`, `mbGame::readTriggers` `0x1400cca30` |
| `include/wse/io/binary_stream.h` | little-endian savegame reader/writer | `rglBinaryStream` vftable `0x1405143d0`; writers `0x1402a1aa0/0x1402a1af0/0x1402a1b50`; readers `0x1402a1d00/0x1402a1d20/0x1402a1d40` |
| `include/wse/vm/operand.h` | tagged operand decode and lvalue write-back | `mbStatement::parseOperand` `0x1401c2100`, `mbStatement::returnValue` `0x1401c2190`, `mbStatementBlock::execute` `0x1401f2530`, `analyze` `0x1401f22a0` |
| `include/wse/vm/slots.h` | `mbSlots` (`std::vector<int64_t>`) get/set semantics | PDB type `0x1b004`; `parseOperand` bounds check; `returnValue` write path |
| `include/wse/vm/statement.h` | statement record and text reader | `mbStatementBlock::readStream` `0x1401f2160`, PDB type `0xb82a` |
| `include/wse/game/trigger.h` | trigger/simple-trigger records, loaders, savegames, and event-key runtime state machines | `0x14023ea20`, `0x14023e750`, `0x14020b9d0`, `0x14020b620`, `0x1400cca30`, `0x1400ccde0`, `0x14023eb80`, `0x14023ed70`, `0x14020bb10`, `0x14023eee0`, `0x14020b6f0`, `0x14020bd10` |
| `include/wse/game/script.h` | `mbScript`/`mbScriptManager` readers, 64-entry name table, and script mapping | `0x14020b210`, `0x14020b330`, `0x14020b510`; globals `0x1406506c0`/`0x1406506c8`/`0x1406506cc`; table `0x1405c7ad0`; formats `0x1405142c0`/`0x1405142d0` |
| `include/wse/vm/wse_opcodes.h` | generated enum of the 433 WSE-extended operation names in the retail dispatch range | `mbStatement::executeWSE` `0x1401e0cf0`; names from `header_operations_addon.py` |
| `include/wse/vm/execute_wse.h` | opcode mask, decoded operand view, dispatch result, and the 2800–2809 handlers | `executeWSE` `0x1401e0cf0` cases at pseudocode lines 2165–2192 and 2390; `mbStatement::execute` `0x1401c21e0` |
| `tests/operand_test.cpp` | host tests for tags, decode, store, slots, tokenizer, statements | local test doubles only |
| `tests/trigger_test.cpp` | host tests for trigger loaders and event-key state machines | local test doubles only |
| `tests/savegame_test.cpp` | host tests for the binary stream and trigger savegames | local test doubles only |
| `tests/script_test.cpp` + `fixtures/scripts.txt` | host tests for script records, header errors, duplicate mapping, and missing-name reporting | local fixture mirroring the retail grammar |
| `tests/wse_test.cpp` | host tests for opcode enum values, shift/bitwise handlers, operand bounds, and opcode masking | local test doubles only |

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

Booleans accept `true`/`false` case-insensitively, otherwise a nonzero decimal integer. The retail
float reader uses `strtod`; this port uses `std::from_chars` and rejects trailing garbage. Valid
retail module files parse identically.

## Runtime

`execute_trigger` / `execute_trigger_manager` and their simple-trigger counterparts reproduce the
inlined retail state machines. The `int` argument is an **event key**, not a clock: non-negative
keys run interval triggers from `g_timersTotal`, while negative keys match the rounded negative
check interval (`-112` = agent exit status, `-105`/`-106` = ranged attack, `-26`/`-56` = death, …).
Conditions and consequences are supplied as callbacks because `mbStatementBlock::execute` is not
reconstructed yet; the timer, status, and event-key transitions are exact.

## Savegames

`rglBinaryStream` stores raw little-endian values with no header or per-value metadata. Trigger
savegames are an `int32` count, then per manager trigger `int32` status plus `int64` check, delay,
and rearm timer values; simple triggers store one `int64` timer value. The portable
`read_trigger_savegame`/`write_trigger_savegame`/`read_simple_trigger_savegame` use
`SavegameResult`: `count_exceeded` reproduces the retail message-box path while consuming every
entry, and `negative_count`/`truncated` reject malformed input. Retail performs no bounds checks on
binary reads, so truncated files would read past the buffer there.

## Scripts

`scripts.txt` is `scriptsfile version <int32>` followed by the script count and that many
`<id> <interval-seconds> <statement block>` records. `mbScriptManager::readStream` discards both
header strings and the version, replaces the script array, parses every record, then maps the 64
static names. Mapping uses exact string equality and the first script in file order wins, so
duplicate ids resolve to the earliest record. A missing name maps to `-1` and reports through
`ScriptMapCallbacks::reportMissing` (retail logs `"Unable to map script %s"`). Each statement block
id becomes `"Script [<index>] <id>"`.

The portable reader was also run against the retail 3,467,512-byte `scripts.txt` sample: 1,755
scripts, every token consumed, 52 of 64 names mapped.

## WSE extended opcodes

`docs/wse_opcodes.json` and `docs/WSE_OPCODES.md` map every SDK operation in the retail extended range
2800–5202 to the `executeWSE` handler that implements it: 433/433 operations are covered (424 switch
cases and 9 equality-tested cases, with no unknown or unhandled opcode in range). Each entry records the
pseudocode line and the handler's call extent. `tools/extract_wse_opcodes.py` regenerates both files and
`include/wse/vm/wse_opcodes.h` from the 9,245-line pseudocode dump and the public
`header_operations_addon.py` names.

`execute_wse` masks the opcode with `0x0FFFFFFF` and currently implements the arithmetic/bitwise group
2800–2809: arithmetic right shift (`val_shr`/`store_shr`), logical right shift
(`val_lshr`/`store_lshr`), left shift (`val_shl`/`store_shl`), xor (`val_xor`/`store_xor`), and
complement (`val_not`/`store_not`). Shift counts are masked with `& 0x3F`; `store_*` reads the value
from operand 1 and leaves operand 0 as the destination lvalue. Every other mapped opcode returns
`WseDispatch::unimplemented` until its handler is transcribed.

## Safety deviations

The retail code trusts operand/statement/script/trigger counts and timer indices. This port rejects
negative counts, operands above 32, oversized arrays, out-of-range timer indices, and truncated
binary savegames; none of these checks changes valid module behaviour.

## Build and test

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1
```

Expected tail:

```
operand tests: PASS
trigger tests: PASS
savegame tests: PASS
script tests: PASS
wse tests: PASS
engine host tests: PASS
```

The Android half of the gate (`clang++ -target aarch64-linux-android29 -Wall -Werror`) cannot run
until an NDK is installed; see `STATUS.md`.
