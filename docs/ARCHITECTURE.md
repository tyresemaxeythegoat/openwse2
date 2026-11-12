# WSE2 Architecture (Reconstructed)

Status: Phase 1 — binary triage complete; script statement VM parse, module text readers, trigger runtime, and binary savegames recovered.
Last updated: 2026-11-12.

This document records only facts derived from the shipped x64 binary and its PDB, or from the public
WSESDK headers. Anything not yet proven by decompilation is marked **open**.

## 1. Evidence provenance

| Artifact | Method | Result |
| --- | --- | --- |
| `mb_warband_wse2_x64.exe` | REA workflow / Ghidra 12.1.4 | SHA-256 `c25b51a42d1b504e2291b66c581c79ca613802d0674618b6d1d7bb7441acdd2b`; PE32+ `amd64`; image base `0x140000000`; 9 segments; **19,759 procedures**; **7,157 strings** (evidence `ev_1665f9ad51202515ef1aacbb7bf7b387cca77cbc8d89b9f1aafe2a938dd5db43`) |
| Imports | REA `inspect_pe_imports` | **21 static + 2 delay-load DLLs, 542 functions** (evidence `ev_db4cc10aa916c9f0b08683c292a14a086adf2d721408cc587a76f6e98879fd70`) |
| `mb_warband_wse2_x64.pdb` | `tools/pdb_symbols.py` (MSF 7.0 + TPI + DBI) | GUID `4029ca80-41b6-484a-bfa3-e26525c7efc7`, age 3, RSDS; **154,753 TPI records**; **14,877 sized class/struct/union/enum layouts** (`types_x64.json`) |
| PDB symbols | `tools/pdb_symbols.py --globals` with PE section mapping | **47,947 symbols** with VAs where applicable: 22,600 public, 1,167 global, 14,701 constant, 8,595 UDT, 884 local (`globals_x64.json`) |
| RTTI + vtables | `tools/pe_evidence.py`, `tools/vtables.py` | 163 RTTI descriptors, mostly `std`/`TiXml`; **281 pointer runs ≥ 4** in `.rdata`/`.data`; **209** identified by destructor symbol; **75** belong to `rgl*`/`mb*`/`WSE*` classes (`vtables_x64.json`) |
| Public SDK | `WSE-master/WSESDK` (`header_operations.py`, Lua headers) | opcode names/IDs; used only as an external cross-reference, not copied code |

The PDB is treated as authoritative structural evidence. The game was compiled without RTTI for its
own classes, so class identity comes from PDB public symbols and vtable first entries
(`??_E<Class>`, `??_G<Class>`, `??1<Class>`), not from `type_info` names.

REA/Ghidra toolchain used for these runs:

- Node `26.12.1`, JDK `25.0.5+8`, Ghidra `12.1.4`, all extracted under `C:\rea-tools` after repeated
  `%TEMP%` wipes.
- REA invoked from `%TEMP%\opencode\tools\rea\node_modules\rea-agents` via `scripts/rea.mjs`.
- 7,086 PDB-derived function names seeded into Ghidra (`seeds.tsv`).

## 2. Type inventory

Counts of sized types by name prefix (14,877 total):

| Prefix | Count | Interpretation |
| --- | ---: | --- |
| `rgl*` | 374 | RGL platform layer (strings, streams, renderer, widgets, resources) |
| `mb*` | 213 | Mount & Blade game/module layer |
| `WSE*` | 24 | WSE2 extensions (Lua manager, dynamic arrays, M&B net) |
| `bt*` | 393 | Bullet Physics (statically linked) |
| `FMOD*` | 25 | FMOD Ex audio |
| `TiXml*` | 19 | TinyXML |
| `Json*` | 36 | JSON parser/serializer (jsoncpp-style diagnostics) |
| `mu*` | 26 | muParser expression engine |
| `lua*` | 3 | Lua 5.1 C API bindings |
| `std::*` | 8,310 | MSVC STL (`std::unordered_map`, `std::vector`, `std::basic_string`) |
| `Concurrency`/`ATL` | 306 / 229 | MSVC runtime/concurrency and ATL |

This supersedes the earlier ROADMAP assumptions of Havok and LuaJIT: the binary links **Bullet
Physics**, **FMOD Ex**, **TinyXML**, and stock **Lua 5.1** (`lua5164.dll`, 103 imports).

## 3. Subsystem map

| Layer | Representative classes | Evidence |
| --- | --- | --- |
| Platform / RGL | `rglString`, `rglStream`, `rglRenderer`, `rglWindowManager`, `rglGraphicsContext`, `rglWidget`, `rglMesh`, `rglMaterial` | PDB layouts; 75 game vtables |
| Module system | `mbScriptManager`, `mbScript`, `mbStatementBlock`, `mbTriggerManager`, `mbTrigger`, `mbSimpleTriggerManager`, `mbSimpleTrigger` | PDB layouts; public function VAs |
| Game | `mbGame`, `mbMission`, `mbMissionTemplate`, `mbAgent`, `mbParty`, `mbFaction`, `mbItem`, `mbTroop` | PDB layouts |
| Lua extensions | `WSELuaManager`, `gameOperation`, `gameConstTable`, `WSEDynMultiArray` | PDB layouts; `g_luaManager` |
| Physics | `bt*` (393 types) | PDB type inventory |
| Audio | `FMOD*` (25 types), `fmodex64.dll` (53 delay imports) | PDB + imports |
| XML / config | `TiXml*` (19 types), `rglConfig`, `rglSectionlessIni` | PDB + `module.ini` string refs |
| Networking | `WS2_32.dll` (37), `WINHTTP.dll` (11), `WININET.dll`, `urlmon.dll` | imports |
| Rendering | `d3d9.dll`, `d3dx9_43.dll` (15), `rglRenderer` (57,232 bytes) | imports + PDB |

## 4. Core layouts (PDB TPI evidence)

All offsets below are byte offsets on x64. `type_index` refers to the PDB TPI type stream.

### `rglString` — 72 bytes, type 0x35f5

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_cursor` | `int32_t` |
| 0x08 | `m_buffer` | `char*` |
| 0x10 | `m_bufferLength` | `int32_t` |
| 0x14 | `m_strLength` | `int32_t` |
| 0x18 | `m_staticBuffer` | `char[48]` |

This is a small-string-optimized string: 48 inline bytes, heap buffer at 0x08 once it grows.

### `rglStream` — 160 bytes, type 0x1b18a

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | vftable | — |
| 0x08 | `m_path` | `rglString` |
| 0x50 | `m_autoFlush` | `bool` |
| 0x54 | `m_cursor` | `int32_t` |
| 0x58 | `m_contents` | `rglString` |

`rglStream` is an in-memory stream over an `rglString`, not a `FILE*` wrapper. Its PDB method set is
`readStream/readString/readInt32/readUInt32/readInt64/readUInt64/readByte/readBool/readFloat` plus
the matching writers and `open/tryOpen/close/isOpen/clear/freeMemory/reset/atEnd/flush`.

These concrete subclasses matter:

| Class | Size | vftable | Adds |
| --- | ---: | --- | --- |
| `rglTextStream` | 160 | inherited | token reader implementations (`readFloat/readInt64/readUInt32/readString`); no standalone vftable is emitted |
| `rglInputTextFileStream` | 176 | `0x14051c0b0` | `m_file` `FILE*` at `+0xA0`, `m_closed` `bool` at `+0xA8`; vtable `+0x50..+0x90` are the `rglTextStream` readers |
| `rglOutputTextFileStream` | 176 | `0x14051c2b0` | `m_file`/`m_closed`/`m_fileOpen` at `+0xA0/+0xA8/+0xA9`; vtable `+0x00..+0x48` are the `rglTextStream` writers; read slots are stubs |
| `rglBinaryStream` | 160 | `0x1405143d0` | 30-slot raw savegame stream; no text parsing, no per-value header |

`rglInputTextFileStream::tryOpen` reads the file into `m_contents`; every reader then skips bytes
`<= 0x20`, takes the next token, and parses it. Module files (`scripts.txt`, `triggers.txt`,
`simple_triggers.txt`, `mission_templates.txt`, …) are whitespace-delimited decimal text. The
separate `rglBinaryStream` (vftable `0x1405143d0`, 30 slots) is used for savegames, not module
files. The binary readers use the embedded `m_contents.m_cursor` at `+0x58` and buffer at `+0x60`;
`rglStream::m_cursor` at `+0x54` is the text-stream cursor. Ghidra names the 4-byte reader
`FUN_1402a1d00` rather than `readInt32` because it did not split the adjacent 20-byte entries.

Exact reader behaviour (`ev_cebc7ae0…e8c8d3`, 18/18 functions):

| Slot | Method | Behaviour |
| --- | --- | --- |
| `+0x58` | `readFloat` | `strtod(token, nullptr)` (0x1404808d8), cast to float |
| `+0x60` | `readBool` | `_stricmp("true"/"false")`, otherwise `strtol(token,10) != 0` |
| `+0x68` | `readUInt64` | `rglString::toUInt64Stream` decimal parser |
| `+0x70` | `readInt64` | `rglString::toInt64Stream` decimal parser |
| `+0x78` | `readUInt32` | 32-bit decimal loop (same code as `readInt32`) |
| `+0x80` | `readInt32` | 15-character manual loop with a leading `-` |
| `+0x88` | `readByte` | same manual decimal loop, 8-bit |
| `+0x90` | `readString` | copies the raw token, no quote stripping |

Every reader first skips bytes `<= 0x20` and calls `rglString::getNextToken`; reading past EOF
raises `"Unexpected End of File while reading file: %s"` with `m_path`. Writers append the decimal
text and a single `0x20` separator; `writeBool` emits `0`/`1`. The retail parsers are lenient
(`strtod` accepts a numeric prefix, the integer loops accept non-digits), so the portable reader
parses strictly and rejects malformed tokens instead.

### `mbStatementBlock` — 96 bytes, type 0x19048

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_statements` | `mbStatement*` |
| 0x08 | `m_numStatements` | `int32_t` |
| 0x0c | `m_analyzed` | `bool` |
| 0x10 | `m_numLocalVariablesUsed` | `int32_t` |
| 0x14 | `m_luaHookRef` | `int32_t` |
| 0x18 | `m_id` | `rglString` |

Methods: `readStream`, `analyze`, `execute`, `startLoop`, `endLoop`, `canLoop`.

### `mbStatement` — 272 bytes, type 0xb82a

| Offset | Member | Type |
| ---: | --- | --- |
| 0x000 | `m_opcode` | `uint32_t` |
| 0x004 | `m_numOperands` | `int32_t` |
| 0x008 | `m_operands` | `__int64[32]` |
| 0x108 | `m_endStatement` | `int32_t` |

Methods: `readStream`, `parseOperand`, `returnValue`, `execute`, `executeWSE`, `arrayCompare`.
This is the decoded instruction record inside `mbStatementBlock::m_statements`; `executeWSE` is the
entry point for WSE2-extended opcodes. `gameOperation` (8 bytes: `opcode` at 0, `flags` at 4) is the
dispatch-table entry type, and `gameConstTable` (56 bytes) holds a named `std::vector<gameConst>`.

### `mbScript` — 176 bytes, type 0x1f6c4

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_id` | `rglString` |
| 0x48 | `m_interval` | `float` |
| 0x50 | `m_statementBlock` | `mbStatementBlock` |

Methods: `readStream(rglStream&, int)`, `execute()`.

### `mbScriptManager` — 272 bytes, type 0xadfd

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_scripts` | `mbScript*` |
| 0x08 | `m_numScripts` | `int32_t` |
| 0x0c | `m_mappedScripts` | `int32_t[64]` |

Methods: `readStream(rglStream&)`, `mapScripts()`. Global instance `g_scriptManager` at
`0x1406506c0`.

### `mbTrigger` — 256 bytes, type 0x77c9

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_checkInterval` | `float` |
| 0x04 | `m_delayInterval` | `float` |
| 0x08 | `m_rearmInterval` | `float` |
| 0x0c | `m_status` | `int32_t` |
| 0x10 | `m_conditionsStatementBlock` | `mbStatementBlock` |
| 0x70 | `m_consequencesStatementBlock` | `mbStatementBlock` |
| 0xd0 | `m_checkTimer` | `rglAppTimer` |
| 0xe0 | `m_delayTimer` | `rglAppTimer` |
| 0xf0 | `m_rearmTimer` | `rglAppTimer` |

Methods: `readStream(rglStream&, char const*, int)`, `execute(int)`, savegame read/write, `setTimer`,
`randomizeIntervals`.

### `mbTriggerManager` — 24 bytes, type 0x1fcfd

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_numTriggers` | `int32_t` |
| 0x08 | `m_triggers` | `mbTrigger*` |
| 0x10 | `m_defaultTimerNo` | `int32_t` |

Methods: `readStream(rglStream&, char const*)`, `readSavegameStream`, `writeSavegameStream`,
`setTimer`, `randomizeIntervals`, `reset`, `execute(int)`, `hasTrigger`, `addTrigger`,
`removeTrigger`.

### `mbSimpleTrigger` — 120 bytes, type 0x71a6

| Offset | Member | Type |
| ---: | --- | --- |
| 0x00 | `m_interval` | `float` |
| 0x08 | `m_statementBlock` | `mbStatementBlock` |
| 0x68 | `m_timer` | `rglAppTimer` |

### `mbSimpleTriggerManager` — 24 bytes, type 0x1e676

Same three-member shape as `mbTriggerManager` with `mbSimpleTrigger*` elements.

### Trigger load flow (pseudocode-confirmed)

`mbGame::readTriggers` (`0x1400cca30`) and `mbGame::readSimpleTriggers` (`0x1400ccde0`) both:

1. Build an `rglInputTextFileStream` (vftable `0x14051c0b0`).
2. Resolve `getModuleFilePath("triggers.txt")` / `"simple_triggers.txt"` and call `tryOpen` (slot
   `+0xA0`). Failure logs `"Unable to open file %s"`.
3. Discard exactly three whitespace-delimited header tokens; EOF logs
   `"Unexpected End of File while reading file: %s"`.
4. Call the manager reader with the literal `"Game"` (`0x14050a958`) or `"Simple"` (`0x14050a974`).

`mbTriggerManager::readStream` (`0x14023ea20`):

1. Read `int32_t m_numTriggers` (slot `+0x80`).
2. Replace `m_triggers` with a `count * 0x100` array of `mbTrigger` (array cookie + constructor).
3. For each index, seed all three `rglAppTimer`s with `m_defaultTimerNo` and
   `g_timersTotal[m_defaultTimerNo]` (`0x1405d0ef0`, `uint64[]`), then call
   `mbTrigger::readStream(trigger, stream, prefix, index)`.

`mbTrigger::readStream` (`0x14023e750`) order:

1. `readFloat` ×3 → `m_checkInterval`/`m_delayInterval`/`m_rearmInterval`.
2. `mbStatementBlock::readStream` → conditions at `+0x10`.
3. `mbStatementBlock::readStream` → consequences at `+0x70`.
4. Ids via `rglFormat("%s Trigger [%d] Conditions")` into the conditions `m_id` and
   `"%s Trigger [%d] Consequences"` into the consequences `m_id`.

`mbSimpleTriggerManager::readStream` (`0x14020b9d0`) is the same with `count * 0x78` records and one
timer per record. `mbSimpleTrigger::readStream` (`0x14020b620`) reads one float at `+0x0`, the
statement block at `+0x8`, then sets the id `"%s Trigger [%d]"`.

Safety: both managers index `g_timersTotal` with an unchecked `m_defaultTimerNo`. The Android port
rejects an out-of-range index instead of reading past the array.

### Trigger runtime (pseudocode-confirmed)

`mbTriggerManager::execute(int)` (`0x14023eee0`) iterates `m_triggers` with stride `0x100` and
**inlines** the `mbTrigger` state machine. The PDB declares `mbTrigger::execute`, but no separate
public symbol is emitted.

The integer argument is a trigger event key, not a clock:

- `key >= 0` enables interval triggers. `mbGame::frameMove` passes `1`, `mbMission::frameMove`
  passes `0x10`, and the slow-motion branch passes `0x10001`; the value itself is only tested for
  sign.
- `key < 0` matches an event trigger. Confirmed call sites: `mbAgent::updateExitStatus` `-112`,
  `updateRangedAttackAction` `-105`/`-106`, `die` `-26`/`-56`, `knockDown` `-27`,
  `updateBlow` `-28`, `fillMovementCapsule` `-110`, `fillCollisionCapsule` `-109`, `turn` `-102`,
  `playActionSound` `-113`.

Timers advance in `g_timersTotal` units of 100000 per second:
`elapsed = (float)(now - stored) / 100000.0`.

Per trigger:

1. status 2 (rearm): if `rearmInterval <= elapsed(rearmTimer)`, set status 0 and
   `checkTimer.value = now[checkTimer.timerNo]`.
2. status 0 (idle):
   - If `checkInterval <= -0.1`, compare `interval_key(checkInterval)` with the key. When they match
     and the conditions block returns true, set status 2, `rearmTimer.value = now`, and execute the
     consequences immediately.
   - Otherwise, when `key >= 0` and `checkInterval <= elapsed(checkTimer)`, set
     `checkTimer.value = now`; on conditions success set status 1 and `delayTimer.value = now`.
3. status 1 (delay): if `delayInterval <= elapsed(delayTimer)`, set status 2,
   `rearmTimer.value = now`, and execute the consequences.

`interval_key` is `floor(x)` plus one only when `x - floor(x) > 0.5`; exact halves round toward
negative infinity.

`mbSimpleTrigger::execute` (`0x14020b6f0`): if `interval < -1.0`, require
`interval_key(interval) == key`; otherwise require `key >= 0` and `interval <= elapsed(timer)`, then
latch `timer.value = now`. Execute the single block; the return value is ignored.
`mbSimpleTriggerManager::execute` (`0x14020bd10`) aborts its loop when the key is odd and positive
and a game-event queue transition is detected (`DAT_1405d0f28 + 0x4a0` vs `+0x4a8`, tail int
`0x12`); that check needs `mbGame` state and is not reconstructed yet.

### Trigger savegames

`rglBinaryStream` (vtable `0x1405143d0`, 30 slots; writer evidence `ev_24eecfa4…f18c0b6`,
reader evidence `ev_130d8023…2c62e6`):

- The stream stores raw values with no header, version, length, or checksum. PDB aliases share
  addresses: `+0x20` `writeInt64/writeUInt64`, `+0x30` `writeInt32/writeUInt32/writeFloat`,
  `+0x38` `writeBool/writeByte`, `+0x68/+0x70` `readUInt64/readInt64`, `+0x80`
  `readInt32/readUInt32/readFloat`, `+0x88` `readBool/readByte`, `+0x90` `readString`.
- `writeFloat` (`0x1402a1aa0`) copies four raw bytes into `m_contents`, `writeInt64`
  (`0x1402a1af0`) eight, and `writeBool` (`0x1402a1b50`) one. `readInt32` (`0x1402a1d00`) loads
  four raw bytes from `m_contents.m_buffer` (`+0x60`) at the embedded `m_contents.m_cursor`
  (`+0x58`) and advances by four; `readByte` (`0x1402a1d20`) and `readInt64` (`0x1402a1d40`) are
  the same pattern with widths one and eight. The x86-64 copies are little-endian.
- Strings are an `int32` byte length followed by the bytes with no terminator: `writeString`
  (`0x1402a1c30`), `writeCString` (`0x1402a1ba0`), and `readString` (`0x1402a1d60`, which reads
  the length through slot `+0x80`). Trigger savegames do not use strings.
- The retail readers perform no bounds check. The portable reader rejects truncated input.
- `mbTriggerManager::writeSavegameStream` (`0x14023ed70`): `writeInt32(count)`; per trigger
  `writeInt32(status)`, `writeInt64(checkTimer.value)`, `writeInt64(delayTimer.value)`,
  `writeInt64(rearmTimer.value)`.
- `mbTriggerManager::readSavegameStream` (`0x14023eb80`): `readInt32(count)`; if the file count
  exceeds the manager count, call `rglError` (`0x140314e30`) with
  `"Load error: Number of triggers in file: %d, exceeds %d"`. `rglError` formats the message,
  shows a message box, and returns, so the loop still reads status and the three 64-bit values for
  every file entry and discards surplus entries. Timer numbers are not saved.
- `mbSimpleTriggerManager::readSavegameStream` (`0x14020bb10`): `readInt32(count)`, error
  `"Load error: Number of simple triggers in file: %d, exceeds %d"`, then one `int64` timer value
  per trigger. No write counterpart appears in the binary.

Portable reconstruction: `engine/include/wse/io/binary_stream.h` plus
`read_trigger_savegame`/`write_trigger_savegame`/`read_simple_trigger_savegame` in
`engine/include/wse/game/trigger.h`. `SavegameResult::count_exceeded` corresponds to the retail
message-box path while still consuming every entry; `negative_count` and `truncated` are safety
deviations for malformed input.

### `WSELuaManager` — 21,504 bytes, type 0xadaf

| Offset | Member | Type |
| ---: | --- | --- |
| 0x0000 | `luaState` | `lua_State*` |
| 0x0008 | `operationMap` | `std::unordered_map<std::string, gameOperation*>` |
| 0x0048 | `gameConstTables` | `std::vector<gameConstTable>` |
| 0x0060 | `gvarMap` | `std::unordered_map<std::string, int>` |
| 0x00a0 | `operationHookLuaRefs` | `int32_t[5300]` |
| 0x5370 | `tStart` | `std::chrono::system_clock::time_point` |
| 0x5378 | `game_fail_stack` | `std::vector<bool>` |
| 0x5398 | `luaContext` | `int32_t` |
| 0x539c | `luaStateIsReady` | `bool` |
| 0x53a0 | `user_dir` | `std::string` |

The `operationHookLuaRefs[5300]` array proves the extended opcode space has at least 5,300 slots.
Global instance `g_luaManager` at `0x14064b2c0`.

WSESDK cross-reference: `header_operations.py` declares 1,535 operation names (1,489 unique numeric
IDs, 0–5,202), and `header_operations_addon.py` adds 477 declarations (472 unique). The maximum SDK
opcode (5,202) fits inside the 5,300-slot hook array, corroborating that the array is indexed by
opcode ID.

### `mbGame` — 94,536 bytes, type 0x1b936 (selected members)

| Offset | Member | Type |
| ---: | --- | --- |
| 0x0008 | `m_savegameName` | `rglString` |
| 0x0058 | `m_moduleName` | `rglString` |
| 0x00a0 | `m_modulePath` | `rglString` |
| 0x0188 | `m_classNames` | `rglString[648]` |
| 0x13df8 | `m_triggers` | `mbTriggerManager` |
| 0x13e10 | `m_simpleTriggers` | `mbSimpleTriggerManager` |

Global instance `g_game` at `0x1405d0f28`; `g_mission` at `0x1405d0f30`; `g_missionTemplates` at
`0x1405d0f20`.

## 5. Module/script entry points

Verified public symbols (mangled names preserve the exact calling convention):

| Address | Symbol | Role |
| --- | --- | --- |
| `0x1400cca30` | `mbGame::readTriggers()` | loads `triggers.txt` into `m_triggers` |
| `0x1400ccde0` | `mbGame::readSimpleTriggers()` | loads `simple_triggers.txt` into `m_simpleTriggers` |
| `0x1400da780` | `mbGame::executeScript(int,int,int)` | executes a module-system script |
| `0x1400da810` | `mbGame::executeMappedScript(int,int,int)` | mapped-script overload |
| `0x1400da8a0` | `mbGame::executeMappedScript(int,int,int,int)` | mapped-script overload |
| `0x1400da940` | `mbGame::executeMappedScript(int,int,int,int,int,int,int)` | mapped-script overload |
| `0x14020b210` | `mbScript::readStream(rglStream&, int)` | parses one script |
| `0x14020b330` | `mbScriptManager::readStream(rglStream&)` | parses `scripts.txt` |
| `0x14020b510` | `mbScriptManager::mapScripts()` | builds `m_mappedScripts[64]` |
| `0x14023e750` | `mbTrigger::readStream(rglStream&, char const*, int)` | parses one trigger |
| `0x14023ea20` | `mbTriggerManager::readStream(rglStream&, char const*)` | parses `triggers.txt` |
| `0x14023eee0` | `mbTriggerManager::execute(int)` | per-frame trigger evaluation |
| `0x14020b620` | `mbSimpleTrigger::readStream(rglStream&, char const*, int)` | parses one simple trigger |
| `0x14020b9d0` | `mbSimpleTriggerManager::readStream(rglStream&, char const*)` | parses `simple_triggers.txt` |
| `0x14020bd10` | `mbSimpleTriggerManager::execute(int)` | per-frame simple-trigger evaluation |
| `0x1403275d0` | `WSELuaManager::doMainScript()` | Lua main-script hook |
| `0x14032f710` | `lc_hookScript(lua_State*)` | Lua C hook for script execution |

String evidence in `.rdata`:

| Address | Literal |
| --- | --- |
| `0x1405068b8` | `scripts.txt` |
| `0x14050a948` | `triggers.txt` |
| `0x14050a960` | `simple_triggers.txt` |
| `0x1405042a8`, `0x140515649` | `module.ini` |
| `0x1405134d0` | `Unrecognized opcode %d` |
| `0x1405230d0` | `opcode %d out of range` |
| `0x1405142c0` | `Script [%d] %s` |
| `0x1405142d0` | `Unable to map script %s` |

### Pseudocode-confirmed parse flow (Ghidra 12.1.4)

`mbScriptManager::readStream` (`0x14020b330`, pseudocode `decompile-index.json` entry):

1. Calls stream vtable `+0x90` twice into a temporary `rglString` (the values are consumed but not
   stored by the manager). In the input text stream (`0x14051c0b0`), `+0x90` is `readString`, so the
   discarded values are the `scriptsfile`/`version` header words.
2. Calls stream vtable `+0x80` twice into 4-byte integers; the second destination is
   `g_scriptManager.m_numScripts` (`0x1406506c8`). Both are read as 32-bit integers.
3. Deletes the old `m_scripts` array with destructors and `operator delete[]`.
4. Allocates `8 + m_numScripts * 176` bytes (array cookie + `mbScript[]`), constructs the array, and
   calls `mbScript::readStream(&m_scripts[i], stream, i)` for every index.
5. Calls `mbScriptManager::mapScripts()`.

`mbScript::readStream` (`0x14020b210`) reads, in order:

1. `readString` into `m_id` (stream vtable `+0x90`).
2. `readFloat` into `m_interval` at `+0x48` (stream vtable `+0x58`).
3. `mbStatementBlock::readStream` into `m_statementBlock` at `+0x50`.
4. Formats `"Script [%d] %s"` with the script index and `m_id`, then stores it in
   `m_statementBlock.m_id` at `+0x68` (`mbScript + 0x50 + 0x18`).

`mbScriptManager::mapScripts` (`0x14020b510`) walks a static 64-entry name table at
`0x1405c7ad0` (first entries verified as `game_start`, `game_quick_start`,
`game_set_multiplayer_mission_end`, `game_get_console_command`, …) and linearly matches each name
against every script's `m_id` (`m_id.m_buffer` at `+0x8`, `m_id.m_strLength` at `+0x14`). It writes
the script index into `m_mappedScripts` (`0x1406506cc`, 64 × `int32_t`), or `-1` when absent, and logs
`"Unable to map script %s"` on failure.

### Statement VM entry points

| Address | Symbol | Signature (mangled-name evidence) |
| --- | --- | --- |
| `0x1401c2100` | `mbStatement::parseOperand` | `int64_t(int64_t*, int, int&)` |
| `0x1401c2190` | `mbStatement::returnValue` | `void(int64_t*, int64_t const&)` |
| `0x1401c21e0` | `mbStatement::execute` | `bool(int64_t*, int, bool)` |
| `0x1401e0cf0` | `mbStatement::executeWSE` | `bool(int64_t*, int64_t*, int*, int*, int)` |
| `0x1401f1400` | `mbStatement::arrayCompare` | `bool(int*, int*, cmp_operation)` |
| `0x1401f2160` | `mbStatementBlock::readStream` | `void(rglStream&)` |
| `0x1401f22a0` | `mbStatementBlock::analyze` | `void()` |
| `0x1401f2530` | `mbStatementBlock::execute` | `bool(int,int,int,int64_t*, mbCallStack*)` |
| `0x1401f2df0` | `mbStatementBlock::startLoop` | `void(int64_t*, mbLoopManager&, int&)` |
| `0x1401f3470` | `mbStatementBlock::endLoop` | `void(int64_t*, mbLoopManager&, int&)` |
| `0x1401f3a60` | `mbStatementBlock::canLoop` | `bool(int64_t*, mbLoopManager&)` |
| `0x140322760` | `WSELuaManager::OnOperationMgrExecute` | `bool(mbStatementBlock*, int&, int64_t*, bool&)` |

`WSELuaManager::OnOperationMgrExecute` is the bridge between the native statement VM and the WSE
operation manager. Its pseudocode confirms that it reads a function from the Lua registry
(`lua_rawgeti(L, LUA_REGISTRYINDEX, hookRef)`), pushes the parsed operands as integers, calls it with
`lua_pcall(L, operandCount, LUA_MULTRET, 0)`, and interprets the returned boolean as either an
override value or a "handled" flag.

### Statement encoding (pseudocode-confirmed)

Module files are **whitespace-delimited decimal text**, not binary records. `mbGame::readTriggers`
(`0x1400cca30`) and `mbGame::readSimpleTriggers` (`0x1400ccde0`) construct an
`rglInputTextFileStream` (vftable `0x14051c0b0`), open `triggers.txt`/`simple_triggers.txt`,
discard three header tokens (`triggersfile version 1` in the retail files) with
`rglString::getNextToken`, then hand the stream to the manager reader.

`rglInputTextFileStream` derives from `rglTextStream`; both keep the 160-byte `rglStream` base and
the file stream adds `FILE* m_file` at `+0xA0` and `bool m_closed` at `+0xA8`. The readers are token
parsers, proven from the `rglInputTextFileStream` vtable `0x14051c0b0`:

| Vtable slot | Method | Statement use |
| --- | --- | --- |
| `+0x58` | `readFloat` | trigger/script intervals |
| `+0x70` | `readInt64` | one operand |
| `+0x78` | `readUInt32` | opcode word |
| `+0x80` | `readUInt32` (folded `readInt32`) | operand/statement count |
| `+0x90` | `readString` | script id |

`mbStatementBlock::readStream` (`0x1401f2160`) therefore reads:

1. `int32_t` statement count (`m_numStatements` at `+0x8`).
2. For each statement: `uint32_t` opcode, `int32_t` operand count, then that many `int64_t` decimal
   operands.
3. `m_endStatement` at `+0x108` is initialized to `-1` by the constructor (`0x1401c20f0`) and filled
   by `analyze`; `m_analyzed` is cleared and `m_numLocalVariablesUsed` is seeded to `0x400`.

The in-memory record remains 272 bytes (`uint32` opcode, `int32` count, 32 × `int64` operands,
`int32` end). The original performs **no bounds check** on either count, so a malformed file
overruns the 32-operand array or requests an unbounded allocation. The Android port rejects negative
counts, operand counts above 32, and statement arrays above its cap.

Operand words use a tagged 64-bit encoding (proven in `mbStatementBlock::execute`):

| Bits | Meaning |
| --- | --- |
| 0–55 | payload |
| 56–62 | 7-bit tag (`(value >> 56) & 0x7f`) |

A negative `int64_t` is treated as an untagged immediate (the full 64-bit value is used). Observed
tags:

| Tag | Meaning | Bounds behaviour |
| ---: | --- | --- |
| `0` | immediate value (also all values `<= 0`) | none needed |
| `1` | global variable array `g_globalVariables` at `0x140613830`, 8-byte elements | **not checked** in the original |
| `2` | global slot vector `g_slots` (`mbSlots`, 24 bytes at `0x140613818`) | read checks `m_values` size, out-of-range yields `0`; write goes through `mbSlots::set` |
| `0x11` | local variable `m_locals[index]` | **not checked** on read; `analyze` sizes the array from the maximum index |

`mbStatement::parseOperand` (`0x1401c2100`) implements the decoder exactly:

1. Load `m_operands[index]`.
2. If the value is `<= 0` (signed), leave it as an immediate and report tag `0`.
3. Otherwise tag is `(value >> 56) & 0x7f`, payload is `value & 0x00FFFFFFFFFFFFFF`.
4. Resolve tag `1` from the global array, tag `2` from `mbSlots` (bounds-checked), or tag `0x11` from
   the local array; return the payload unchanged for tag `0`.

`mbStatement::returnValue` (`0x1401c2190`) is the matching write-back path. It treats **operand 0**
as the destination lvalue (tag byte at `this+0xf`, index at `this+8`):

- tag `1` → write `g_globalVariables[index]`
- tag `2` → `mbSlots::set(index, value)` (grows the vector as needed)
- tag `0x11` → write `m_locals[index]`
- anything else → `rglError("Illegal lvalue in statement")`

This is how operation results become the input of a following statement.

`analyze` (`0x1401f22a0`) computes the local-variable count by scanning operands (clamped to 32) for
tag `0x11` and storing `maxIndex + 1`, clamped to `[0x20, 0x400]`, in `m_numLocalVariablesUsed`.
It also resolves control flow: opcode `3` (`try_end`), `4` (`try_begin`), `5` (`else_try`), and the
loop starters `6, 7, 0xb, 0xc, 0x10, 0x11, 0x12` maintain two 128-entry stacks, and every covered
statement gets `m_endStatement` set to the matching exit index (unmatched entries get the statement
count). Nesting beyond 128 is a hard error.

### Opcode word and dispatch

`mbStatement::execute` (`0x1401c21e0`) masks the opcode word with `0x0FFFFFFF`; the top bits are
modifiers:

| Bit | Effect |
| --- | --- |
| `0x40000000` (30) | arms the global "stop/return" latch (`DAT_1405d0d9c`) after execution |
| `0x80000000` (31) | inverts the boolean result |

Dispatch order:

1. Parse every operand with `mbStatement::parseOperand` into parallel `int64_t[]` and `int32_t[]`
   arrays.
2. If the statement is not WSE-disabled and `operationHookTable[opcode] != -2`, call
   `WSELuaManager::OnOperationExecute`; a hook may supply the return value.
3. Opcodes `30..2767` (`0x1e..0xACF`) dispatch through a 2,738-entry jump table of 32-bit RVAs at
   `0x1401de220` (VA base `0x140000000`). The decompiled call passes the second parsed operand;
   the full native handler signature still needs confirmation.
4. Opcodes `>= 2768` (`0xAD0`) go to `mbStatement::executeWSE` (`0x1401e0cf0`), a 381 KB / 9,245-line
   pseudocode function with 430 `case` labels and four nested switch sites. This is the WSE2
   extended opcode implementation and the largest single reconstruction target.

### Block execution

`mbStatementBlock::execute` (`0x1401f2530`) is the interpreter loop:

- Pushes a frame on the global call stack (`DAT_1405d0da0`); frame storage is `0x1C08` bytes with a
  128-entry loop stack of `0x38`-byte frames and a depth field at `+0x1C00`.
- Calls `analyze()` on entry, then optionally zeroes `clamp(m_numLocalVariablesUsed, 0x20, 0x400)`
  qwords of locals.
- If `m_luaHookRef` (`+0x14`) is not `-2`, calls `WSELuaManager::OnOperationMgrExecute` for a
  block-level Lua hook; a `false` return aborts the block.
- Special opcodes handled in the loop:
  - `1` `call_script`: resolves operand 0 through the tag decoder, bounds-checks the script index
    against `m_numScripts`, and recursively executes `g_scriptManager.m_scripts[index] + 0x50`
    (`mbScript` stride `0xB0`, statement block at `+0x50`). Call depth is capped at `0xFF`.
  - `3` `try_end` → `endLoop`; `4` `try_begin` pushes a loop frame; `5` `else_try` jumps to
    `m_endStatement` when the previous condition was true; `8` `break_loop` and `9` `continue_loop`
    scan the loop stack for the enclosing loop frame.
  - `0x15`, `0x16`, `0x17` are script-parameter access/store operations using `parseOperand` and
    `returnValue`; invalid parameter IDs log `"Invalid Script Parameter ID: %d"`.
  - All other opcodes `<= 0x14` route to `startLoop`.
  - Opcodes `>= 0x1E` go to `mbStatement::execute`.

## 6. External dependencies (import evidence)

| DLL | Functions | Kind | Port strategy |
| --- | ---: | --- | --- |
| `KERNEL32.dll` | 223 | static | Replace with C++20 `std::filesystem`, `std::thread`, POSIX/Android shims |
| `lua5164.dll` | 103 | static | Link Lua 5.1.x for arm64 (or a compatible 5.1-compatible VM); **not** LuaJIT |
| `fmodex64.dll` | 53 | delay | Replace with Oboe/SDL audio; FMOD Ex is not redistributable here |
| `USER32.dll` | 40 | static | Replace with SDL2/Android input and windowing |
| `WS2_32.dll` | 37 | static | BSD sockets (available on Android) |
| `d3dx9_43.dll` | 15 | static | Replace D3DX helpers (math, texture, shader) |
| `WINHTTP.dll` | 11 | static | Replace with `HttpURLConnection`/libcurl |
| `ADVAPI32.dll` | 10 | static | Registry/crypto shims; registry not used on Android |
| `steam_api64.dll` | 10 | delay | Exclude; no Steam on Android |
| `dbghelp.dll` | 8 | static | Optional crash reporting; replace with `backtrace`/Breakpad |
| `ole32.dll`, `OLEAUT32.dll` | 5 + 5 | static | COM shims only if required by SDK paths |
| `IMM32.dll` | 4 | static | IME/text input via Android `InputMethodManager` |
| `SHLWAPI.dll`, `SHELL32.dll` | 4 + 2 | static | Replace path/shell helpers |
| `WINMM.dll` | 3 | static | Timers -> `std::chrono`/SDL |
| `XINPUT1_3.dll` | 3 | static | SDL game controller API |
| `PSAPI.DLL`, `GDI32.dll`, `d3d9.dll`, `DINPUT8.dll`, `urlmon.dll`, `WININET.dll` | 1 each | static | Replace per subsystem |

## 7. Android port architecture (planned)

```
Kotlin/Java UI (SAF folder picker, touch overlay, settings)
        │ JNI
C++20 core (libwse2.so, arm64-v8a)
  ├─ rgl platform layer: rglString/rglStream/rglConfig (portable, no Win32)
  ├─ module loader: reads user-selected Modules/ via SAF file descriptors
  ├─ script/trigger VM: mbScriptManager + mbTriggerManager + mbSimpleTriggerManager
  ├─ WSE Lua layer: WSELuaManager + gameOperation table on Lua 5.1
  ├─ renderer: Vulkan (PanVK on Mali) with GLES 3.2 fallback
  ├─ physics: Bullet (arm64 build) — imported type inventory already matches
  └─ audio: Oboe/OpenSL ES
```

Existing `runtime/android` PanVK work remains valid for the renderer path: the loader deliberately
bypasses platform `libvulkan.so`, patches the ICD's `libhardware.so` dependency to `libpanvk_hw.so`,
and requires `extractNativeLibs=true`. It is independent of the reconstructed game code.

## 8. Evidence gaps / next measurements

1. Portable savegame support: the binary `rglBinaryStream` slot semantics and savegame file framing
   (header/version) are documented only at the trigger-record level.
2. `mbSimpleTriggerManager::execute` early-out depends on `mbGame` event-queue state
   (`DAT_1405d0f28 + 0x4a0/0x4a8`, tail int `0x12`); the queue lifecycle is not reconstructed.
3. Locate `gameOperation` table construction in `WSELuaManager` and map opcode IDs to the native jump
   table and `executeWSE` cases.
4. Confirm `rglConfig`/`module.ini` key names and defaults from string xrefs.
5. Confirm renderer resource formats (`.brf`, `.dds`) from `rglMesh`/`rglMaterial` loaders before
   writing asset code.
6. Validate every reconstructed offset against decompiled member accesses before committing C++.

## 9. Evidence index

REA evidence records:

| ID | Operation | Notes |
| --- | --- | --- |
| `ev_1665f9ad51202515ef1aacbb7bf7b387cca77cbc8d89b9f1aafe2a938dd5db43` | `binary_overview` | 19,759 procedures, 7,157 strings |
| `ev_db4cc10aa916c9f0b08683c292a14a086adf2d721408cc587a76f6e98879fd70` | `inspect_pe_imports` | 23 DLLs, 542 functions |
| `ev_7cd99ccc2639d6a0c4aab91f8cf6b1c8bd64971ccab2c1b77e8c0e8d8c157d7a` | `decompile_all --name mbScript` | 5 procedures, 0 failures |
| `ev_8d91b1752f2c9586db78cc7c89b816dabba31faef0df0f5bd42ba27040199b11` | `decompile_all --name mbStatement` | 14 procedures written, 0 failures, 0 skipped |
| `ev_25469ff40260f5b7e5a9bac7c035b8cd2f0d880c536e40f566c9587993184af8` | `decompile_all --address-start 0x1401c20f0 --address-end 0x1401c21c0` | `parseOperand`, `returnValue`, `mbStatement` ctor; 3 written, 0 failures |
| `ev_e3530f99c32b3f59ecc82debcfff9db29d8641fd14d5f7576ccf12eb4cbe8d05` | `decompile_all --name Trigger` | 52 procedures written, 0 failures; trigger layouts, loaders, runtime, and savegames |
| `ev_cebc7ae0bef7bccaddbcbb53c560d5d2020a5acf60c5bc03a77c496077e8c8d3` | `decompile_all --name rglTextStream` | 18 procedures written, 0 failures; exact token reader/writer semantics |
| `ev_24eecfa4fc91587dd7b5b29ca767a78c8747a538e953f37b295746c67f18c0b6` | `decompile_all --name rglBinaryStream` | 9 procedures written, 0 failures; raw 8/4/1-byte writers and length-prefixed strings |
| `ev_130d8023206760c8a441e940113e7a231d384634f4a73d4813a3ae6b412c62e6` | `decompile_all --address-start 0x1402a1d00 --address-end 0x1402a1d7f` | 2 procedures written, 0 failures; 4-byte raw reader (`FUN_1402a1d00`); adjacent 1/8-byte entries confirmed by PDB aliases and disassembly |

Derived PDB/PE artifacts (retained outside the repository; hashes pin the exact inputs):

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `types_x64.json` | 2,965,950 | `4ce032f331547a231dd15e52a400e0e277a38214dceb15b7de72d89aa5fbe078` |
| `globals_x64.json` | 6,778,279 | `4af47515edab25de63e36a1a77b364a011c5e15a93d77d2660c4a970794d446b` |
| `vtables_x64.json` | 483,710 | `f9b531cf244bd382ca2b59f3afb80638b1a46251062792cc5da80f700a54c964` |
| `imports_x64.json` | 69,259 | `8dd4b0747bc0a1a5ff930e8f99ccb073cf861e8ec21de3281d5492762ce2a646` |
| `analysis_x64.json` | 8,200 | `891bceb598a01a0a9d490f926ac6bddf8cf37ba8c4e21a6f4568015beed5344d` |

## 10. Legal guardrail

No game assets, retail binaries, PDBs, or derived disassembly are checked into the repository.
The Android app is BYOA: it asks the user to select a legally owned Warband installation and reads
`Modules/` from that location. No DRM or license check is bypassed.
