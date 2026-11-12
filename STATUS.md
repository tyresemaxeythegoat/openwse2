# STATUS

Last updated: 2026-11-12.
Phase: **1 — Binary Triage & SDK Alignment** (triage complete, script statement VM recovered).

## Passing

| Check | Result | Evidence |
| --- | --- | --- |
| Target extraction | PASS | `mb_warband_wse2_x64.exe` 6,735,360 B, SHA-256 `c25b51a4…acdd2b` |
| REA/Ghidra analysis | PASS | 19,759 procedures, 7,157 strings, image base `0x140000000` |
| PE import inventory | PASS | 23 DLLs (21 static + 2 delay), 542 functions |
| PDB parse (MSF 7.0/TPI/DBI) | PASS | 154,753 TPI records, 14,877 sized layouts |
| PDB symbol map | PASS | 47,947 symbols: 22,600 public, 1,167 global, 14,701 constant, 8,595 UDT, 884 local |
| Vtable scan | PASS | 281 pointer runs; 209 class-identified; 75 `rgl*`/`mb*`/`WSE*` |
| Tracking documents | PASS | `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (19 classes, 1,673 symbols, 75 vtables) |
| `decompile-all --name mbScript` | PASS | 5 procedures written, 0 failed; entry `ev_7cd99ccc2639d6a0c4aab91f8cf6b1c8bd64971ccab2c1b77e8c0e8d8c157d7a` |
| `decompile-all --name mbStatement` | PASS | 14 procedures written, 0 failed, 0 skipped; entry `ev_8d91b1752f2c9586db78cc7c89b816dabba31faef0df0f5bd42ba27040199b11` |
| `decompile-all` operand helpers | PASS | 3 procedures written, 0 failed (`parseOperand`, `returnValue`, ctor); entry `ev_25469ff40260f5b7e5a9bac7c035b8cd2f0d880c536e40f566c9587993184af8` |
| Host PanVK loader/surface test | PASS | `host loader tests: PASS` (legacy renderer path, unaffected) |
| Ghidra toolchain health | PASS | `rea doctor`: ghidra 12.1.4, JDK 25.0.5, native decompiler and native authority all healthy |

## In progress

| Task | State |
| --- | --- |
| Trigger manager pseudocode | Layouts and entry points verified; `mbTriggerManager` export queued |
| `executeWSE` case mapping | 381 KB pseudocode captured (430 cases); individual WSE opcode → handler mapping not yet extracted |

## Confirmed by pseudocode

- `mbScriptManager::readStream` (`0x14020b330`): two `readString` calls (discarded), two 32-bit reads,
  the second into `m_numScripts`; frees the old array, allocates `8 + count*176`, constructs
  `mbScript[]`, calls `mbScript::readStream(index)` per entry, then `mapScripts()`.
- `mbScript::readStream` (`0x14020b210`) order: `m_id` (`readString`), `m_interval`
  (`readFloat` @ `+0x48`), `mbStatementBlock::readStream` (@ `+0x50`), then sets
  `m_statementBlock.m_id` to `rglFormat("Script [%d] %s", index, m_id)`.
- `mbScriptManager::mapScripts` (`0x14020b510`): 64-entry static name table at `0x1405c7ad0`
  (`game_start`, …), linear match on `m_id`, writes index or `-1` into `m_mappedScripts`
  (`0x1406506cc`), logs `"Unable to map script %s"` on failure.
- `mbStatementBlock::readStream` (`0x1401f2160`): reads `int32` count, allocates `count * 0x110`,
  then per statement reads `uint32` opcode, `int32` operand count, and that many `int64` operands;
  sets every `m_endStatement` (`+0x108`) to `-1`. **No bounds check** on either count.
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
- `mbStatementBlock::execute` (`0x1401f2530`): interpreter loop with a `0x1C08`-byte frame,
  128 × `0x38` loop frames, locals zeroing, block-level Lua hook, `call_script` recursion into
  `m_scripts[index] + 0x50` (stride `0xB0`, max depth 255), and special handling for opcodes
  `3/4/5/8/9`, `0x15/0x16/0x17`.
- `WSELuaManager::OnOperationMgrExecute` (`0x140322760`): fetches the hook from the Lua registry,
  pushes operands as integers, `lua_pcall(L, nargs, LUA_MULTRET, 0)`, and interprets the boolean
  result as an override or handled flag.

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

1. Decompile `mbTriggerManager::readStream` (`0x14023ea20`) / `execute` (`0x14023eee0`) and the
   `mbSimpleTriggerManager` equivalents; compare against the proven statement encoding.
2. Extract the WSE opcode → case mapping from `executeWSE` and correlate it with
   `header_operations_addon.py`.
3. Add the first portable C++20 headers for `rglString`, `rglStream`, `mbStatement`,
   `mbStatementBlock`, and `mbScript`, with host unit tests covering the tagged operand decoder.
4. Update `ROADMAP.md` and this file at the next milestone.
