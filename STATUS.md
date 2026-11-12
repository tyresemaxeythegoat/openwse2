# STATUS

Last updated: 2026-11-12.
Phase: **1 — Binary Triage & SDK Alignment** (script VM, module text/binary streams, trigger loaders/runtime/savegames recovered).

## Passing

| Check | Result | Evidence |
| --- | --- | --- |
| Target extraction | PASS | `mb_warband_wse2_x64.exe` 6,735,360 B, SHA-256 `c25b51a4…acdd2b` |
| REA/Ghidra analysis | PASS | 19,759 procedures, 7,157 strings, image base `0x140000000` |
| PE import inventory | PASS | 23 DLLs (21 static + 2 delay), 542 functions |
| PDB parse (MSF 7.0/TPI/DBI) | PASS | 154,753 TPI records, 14,877 sized layouts |
| PDB symbol map | PASS | 47,947 symbols: 22,600 public, 1,167 global, 14,701 constant, 8,595 UDT, 884 local |
| Vtable scan | PASS | 281 pointer runs; 209 class-identified; 75 `rgl*`/`mb*`/`WSE*` |
| Tracking documents | PASS | `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (25 classes, 1,673 symbols, 75 vtables) |
| `decompile-all --name mbScript` | PASS | 5 procedures written, 0 failed; entry `ev_7cd99ccc2639d6a0c4aab91f8cf6b1c8bd64971ccab2c1b77e8c0e8d8c157d7a` |
| `decompile-all --name mbStatement` | PASS | 14 procedures written, 0 failed, 0 skipped; entry `ev_8d91b1752f2c9586db78cc7c89b816dabba31faef0df0f5bd42ba27040199b11` |
| `decompile-all` operand helpers | PASS | 3 procedures written, 0 failed (`parseOperand`, `returnValue`, ctor); entry `ev_25469ff40260f5b7e5a9bac7c035b8cd2f0d880c536e40f566c9587993184af8` |
| `decompile-all --name Trigger` | PASS | 52 procedures written, 0 failed, 0 skipped; entry `ev_e3530f99c32b3f59ecc82debcfff9db29d8641fd14d5f7576ccf12eb4cbe8d05` |
| `decompile-all --name rglTextStream` | PASS | 18 procedures written, 0 failed, 0 skipped; entry `ev_cebc7ae0bef7bccaddbcbb53c560d5d2020a5acf60c5bc03a77c496077e8c8d3` |
| `decompile-all --name rglBinaryStream` | PASS | 9 procedures written, 0 failed; entry `ev_24eecfa4fc91587dd7b5b29ca767a78c8747a538e953f37b295746c67f18c0b6` |
| `decompile-all` binary readers | PASS | 2 procedures written, 0 failed; entry `ev_130d8023206760c8a441e940113e7a231d384634f4a73d4813a3ae6b412c62e6` |
| Binary savegame format | PASS | `rglBinaryStream` raw little-endian 8/4/1-byte values; strings are `int32` length + bytes; no header or checksum |
| Module stream identification | PASS | `0x14051c0b0` = `rglInputTextFileStream` (readers at `+0x58/+0x70/+0x78/+0x80/+0x90`); `0x14051c2b0` = `rglOutputTextFileStream` writers; module files are decimal token text |
| Host PanVK loader/surface test | PASS | `host loader tests: PASS` (legacy renderer path, unaffected) |
| Portable engine host tests | PASS | `operand tests: PASS`, `trigger tests: PASS`, `savegame tests: PASS`, `script tests: PASS`, `wse tests: PASS`, `engine host tests: PASS` (Zig clang++, `-Wall -Wextra -Werror`) |
| WSE extended opcode map | PASS | 433/433 SDK operations 2800–5202 mapped to `executeWSE` handlers (424 switch + 9 equality cases); `docs/WSE_OPCODES.md`, `docs/wse_opcodes.json`, generated `engine/include/wse/vm/wse_opcodes.h` |
| Retail `scripts.txt` parse | PASS | Full 3,467,512-byte sample: 1,755 scripts, every token consumed, 52/64 mapped names (the rest are WSE-only scripts absent from that module) |
| Ghidra toolchain health | PASS | `rea doctor`: ghidra 12.1.4, JDK 25.0.5, native decompiler and native authority all healthy |

## In progress

| Task | State |
| --- | --- |
| Portable trigger runtime | Event-key state machine reconstructed and host-tested; statement conditions/consequences are callbacks until the VM executes blocks |
| Portable trigger savegames | Binary reader/writer and simple-trigger reader reconstructed and host-tested, including count-exceeded consumption and truncation rejection |
| `executeWSE` case mapping | Complete: 433/433 SDK operations 2800–5202 resolved to switch/equality handlers; no unknown in-range opcode |
| Portable `executeWSE` handlers | 25 of 433 opcodes transcribed and host-tested: 2800–2809 shifts/xor/complement and 4400/4405–4417/4438 floating-point load/store/compare/arithmetic; the other 408 report `WseDispatch::unimplemented` |

## Confirmed by pseudocode

- `mbScriptManager::readStream` (`0x14020b330`): two `readString` calls (discarded), two 32-bit reads,
  the second into `m_numScripts`; frees the old array, allocates `8 + count*176`, constructs
  `mbScript[]`, calls `mbScript::readStream(index)` per entry, then `mapScripts()`.
- `mbScript::readStream` (`0x14020b210`) order: `m_id` (`readString`), `m_interval`
  (`readFloat` @ `+0x48`), `mbStatementBlock::readStream` (@ `+0x50`), then sets
  `m_statementBlock.m_id` to `rglFormat("Script [%d] %s", index, m_id)`.
- `mbScriptManager::mapScripts` (`0x14020b510`): 64-entry static name table at `0x1405c7ad0`
  (`game_start` … `wse_initial_window_start`), linear match on `m_id` with the first script in file
  order winning, writes the index or `-1` into `m_mappedScripts` (`0x1406506cc`), and calls `rglInfo`
  with `"Unable to map script %s"` on failure. All 64 names are transcribed in `script.h`.
- Module files are whitespace-delimited decimal text, not binary. `mbGame::readTriggers`
  (`0x1400cca30`) and `readSimpleTriggers` (`0x1400ccde0`) build `rglInputTextFileStream`
  (vftable `0x14051c0b0`), open `triggers.txt`/`simple_triggers.txt`, discard three header tokens
  (`triggersfile version 1`), and pass the stream to the manager.
- `rglStream` (160 B) holds `m_path` at `+0x8`, `m_cursor` at `+0x54`, and the whole file as
  `m_contents` at `+0x58`. `rglTextStream` (160 B) supplies the token readers but emits no
  standalone vftable. `rglInputTextFileStream` (176 B, vftable `0x14051c0b0`) adds `FILE* m_file` at
  `+0xA0` and `bool m_closed` at `+0xA8`; `rglOutputTextFileStream` (176 B, vftable `0x14051c2b0`)
  adds the same plus `m_fileOpen` at `+0xA9`. Reader slots: `+0x50 readStream`, `+0x58 readFloat`,
  `+0x60 readBool`, `+0x68 readUInt64`, `+0x70 readInt64`, `+0x78/+0x80` the folded 32-bit parser,
  `+0x88 readByte`, `+0x90 readString`.
- `rglTextStream` reader behaviour (`ev_cebc7ae0…e8c8d3`): `readFloat` is `strtod(token)` cast to
  float; `readBool` is `_stricmp("true"/"false")` else `strtol(token) != 0`; the integer readers use
  manual decimal loops (15-character cap, leading `-`); `readString` copies the raw token, with no
  quote handling. Writers append a single `0x20` after each value and write booleans as `0`/`1`.
  The portable reader parses strictly and rejects malformed tokens as a safety deviation.
- `mbStatementBlock::readStream` (`0x1401f2160`): reads `int32` statement count, allocates
  `count * 0x110`, then per statement reads `uint32` opcode, `int32` operand count, and that many
  `int64` operand tokens; sets every `m_endStatement` (`+0x108`) to `-1`, clears `m_analyzed`, and
  seeds `m_numLocalVariablesUsed` to `0x400`. **No bounds check** on either count.
- `mbStatementBlock` layout: `m_statements` `+0x0`, `m_numStatements` `+0x8`, `m_analyzed` `+0xc`,
  `m_numLocalVariablesUsed` `+0x10`, `m_luaHookRef` `+0x14`, `m_id` `+0x18` (96 bytes total).
- `analyze` (`0x1401f22a0`): matches opcodes `3/4/5` and loop starters `6,7,0xb,0xc,0x10,0x11,0x12`
  with two 128-entry stacks, writes `m_endStatement` jump targets, caps nesting at 128, and computes
  `m_numLocalVariablesUsed` from operand tag `0x11`, clamped to `[0x20, 0x400]`.
- Operand encoding: 7-bit tag in bits 56–62, 56-bit payload. Tags `0` immediate, `1` global variable
  array `g_globalVariables` (`0x140613830`, unchecked), `2` global `mbSlots` (`0x140613818`,
  read bounds-checked), `0x11` local variable. Negative 64-bit values are untagged immediates.
- `mbStatement::parseOperand` (`0x1401c2100`) implements the decoder: `<= 0` is an immediate,
  otherwise tag = `(value >> 56) & 0x7f` and payload = `value & 0x00FFFFFFFFFFFFFF`.
- `mbStatement::returnValue` (`0x1401c2190`) treats operand 0 as the destination lvalue and writes
  the result to the global array (tag 1), `mbSlots::set` (tag 2), or the local array (tag `0x11`);
  other tags raise `"Illegal lvalue in statement"`.
- `mbStatement` constructor (`0x1401c20f0`) only initializes `m_endStatement` to `-1`; `readStream`
  fills the opcode and operands.
- `mbStatement::execute` (`0x1401c21e0`): parses operands, masks opcode with `0x0FFFFFFF`, calls
  `WSELuaManager::OnOperationExecute` when `operationHookTable[opcode] != -2`, then dispatches
  opcodes `30..2767` through the 2,738-entry RVA jump table at `0x1401de220` and opcodes `>= 2768`
  to `executeWSE`. Bit 31 inverts the result; bit 30 arms the stop latch (`DAT_1405d0d9c`).
- `docs/wse_opcodes.json` / `docs/WSE_OPCODES.md` / generated `wse_opcodes.h` now cover all 433 SDK
  operations in the extended range 2800–5202. 424 are switch cases in `executeWSE`; the other 9 are
  equality-tested handlers at 2800, 3000, 3300, 3600, 3900, 4200, 4400, 4700, and 5000. No in-range
  SDK operation lacks a handler.
- The 2800–2809 group is proven by the pseudocode at lines 2165–2192 and 2390: `val_shr`/`store_shr`
  cast to signed before `>>` (arithmetic); `val_lshr`/`store_lshr` shift the `ulonglong` operand
  directly (logical); `val_shl`/`store_shl` shift left; `val_xor`/`store_xor` combine operands 0/1 or
  1/2; `val_not`/`store_not` complement operand 0 or 1. Every shift masks the count with `& 0x3F`.
- The floating-point cases at lines 5369–8846 index the 128-float array `DAT_140649318`.
  `mbCheckRegister` (`0x1401c0ef0`) is `cmp ecx,0x7f; ja` (unsigned 0–127); a failed check returns
  true without doing anything. `DAT_1406137b8` is the fixed-point scale and is set to `0x64` (100) at
  `0x140070f3a`; `fld` divides by it, `fst` multiplies by it.
- `rglRound64` (`0x1402950d0`) calls `floorf` (`0x1404bd5f8`), subtracts to get the fraction, and adds
  one only when the fraction is greater than `0.5f` (constant at `0x14052c17c`). `rglAbs`
  (`0x140295010`) clears the sign bit; `feq`/`fge`/`fle` compare through it with `FLT_EPSILON`.
- `fclamp` orders its two bounds first (`min`/`max` of operands 2 and 3), then returns the lower bound,
  the value, or the upper bound. `fdiv`/`fmod` report `"Division by zero"`/`"Modulo by zero"` and
  leave the destination unchanged.
- `mbStatementBlock::execute` (`0x1401f2530`): interpreter loop with a `0x1C08`-byte frame,
  128 × `0x38` loop frames, locals zeroing, block-level Lua hook, `call_script` recursion into
  `m_scripts[index] + 0x50` (stride `0xB0`, max depth 255), and special handling for opcodes
  `3/4/5/8/9`, `0x15/0x16/0x17`.
- `WSELuaManager::OnOperationMgrExecute` (`0x140322760`): fetches the hook from the Lua registry,
  pushes operands as integers, `lua_pcall(L, nargs, LUA_MULTRET, 0)`, and interprets the boolean
  result as an override or handled flag.
- `mbTriggerManager::readStream` (`0x14023ea20`): reads `m_numTriggers`, allocates
  `count * 0x100`, seeds all three `rglAppTimer`s per trigger with `m_defaultTimerNo` and
  `g_timersTotal[m_defaultTimerNo]` (`0x1405d0ef0`), then calls `mbTrigger::readStream`.
- `mbTrigger::readStream` (`0x14023e750`): three `readFloat` intervals (`+0x0/+0x4/+0x8`), the
  conditions block (`+0x10`), the consequences block (`+0x70`), then ids
  `"%s Trigger [%d] Conditions"` / `"%s Trigger [%d] Consequences"`.
- `mbSimpleTriggerManager::readStream` (`0x14020b9d0`): the same shape with `count * 0x78` records
  and one timer each; `mbSimpleTrigger::readStream` (`0x14020b620`) reads one float, the statement
  block, and formats `"%s Trigger [%d]"`.
- `mbGame::readTriggers` passes the prefix `"Game"` (`0x14050a958`); `readSimpleTriggers` passes
  `"Simple"` (`0x14050a974`).
- `mbTriggerManager::execute` (`0x14023eee0`) **inlines** the `mbTrigger` state machine; the PDB
  declares `mbTrigger::execute`, but no separate public symbol is emitted. The integer argument is a
  trigger event key: non-negative keys run interval triggers; negative keys match
  `interval_key(checkInterval)`. Confirmed call sites pass `-112` (exit status), `-105`/`-106`
  (ranged attack), `-26`/`-56` (death), `-27` (knockdown), `-28` (blow), `-110`/`-109` (movement and
  collision capsules), `-102` (turn), and `-113` (action sound); frame code passes `1` and `0x10`.
- Trigger statuses are `0` idle, `1` waiting for delay, `2` waiting for rearm. Timers advance in
  `g_timersTotal` units of 100000/second and elapsed is `(float)(now - value) / 100000.0`. Rearm
  expiry resets the check timer; idle conditions success starts the delay; delay expiry runs the
  consequences; a matching negative event key runs the consequences immediately.
- Savegames use `rglBinaryStream` (vtable `0x1405143d0`, 30 slots). Writers store raw
  little-endian values (8 bytes for `writeInt64`, 4 for `writeInt32`/`writeFloat`, 1 for
  `writeBool`/`writeByte`); strings are an `int32` byte count followed by the bytes. There is no
  header, version, or checksum. `mbTriggerManager::writeSavegameStream` (`0x14023ed70`) writes an
  `int32` count, then `int32` status and the `int64` check/delay/rearm timer values per trigger.
  `readSavegameStream` (`0x14023eb80`) reads the same order; when the file count is larger it calls
  `rglError` (`0x140314e30`) with `"Load error: Number of triggers in file: %d, exceeds %d"` (a
  message box that returns) and still reads and discards the surplus entries.
  `mbSimpleTriggerManager::readSavegameStream` (`0x14020bb10`) reads one `int64` timer value per
  record with the analogous `"Load error: Number of simple triggers in file: %d, exceeds %d"`.
  The retail readers do not bounds-check; the portable reader rejects truncation.

## Blocked

| Blocker | Impact |
| --- | --- |
| No Android NDK/SDK, `adb`, or device | Native arm64 build and on-device validation cannot run; only host/static checks |
| REA not registered with OpenCode | REA/Ghidra tools are not exposed through MCP; CLI works directly from `%TEMP%\opencode\tools\rea\node_modules\rea-agents` |

## Environment notes

- `%TEMP%` extraction directories are wiped on server restarts. Node 26, JDK 25, and Ghidra 12.1.4
  are now extracted to the persistent `C:\rea-tools` tree; the zips remain in `%TEMP%\opencode\tools`.
- Required environment for CLI runs:
  `JAVA_HOME=C:\rea-tools\jdk-25.0.5+8`,
  `GHIDRA_INSTALL_DIR=C:\rea-tools\ghidra_12.1.4\ghidra_12.1.4_PUBLIC`.
- `msdia140.dll` is now COM-registered system-wide (`regsvr32`, elevated) from
  `C:\rea-tools\ghidra_12.1.4\ghidra_12.1.4_PUBLIC\Ghidra\Features\PDB\os\win_x86_64\`. Ghidra can
  load the retail PDB directly; the earlier hanging `RegSvr32` dialogs were orphaned manual
  registration attempts and are resolved.
- `decompile-all` refuses a pre-created output directory; let the tool create it. Use `--resume`
  only when a previous export manifest exists.
- Each export re-runs full Ghidra auto-analysis (~15–20 min). Do not run two exports concurrently
  on this 8 GB machine; a second OpenCode session using Ghidra drives free RAM below 1 GB.

## Next concrete task

1. Transcribe the next `executeWSE` handler groups from `docs/WSE_OPCODES.md`, extending the host
   gate as each group lands until all 433 mapped opcodes are implemented.
2. Feed reconstructed statement blocks into `execute_trigger` so conditions/consequences stop being
   test callbacks once `mbStatementBlock::execute` is portable.
3. Update `ROADMAP.md` and this file at the next milestone.
