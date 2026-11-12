# STATUS

Last updated: 2026-11-12.
Phase: **1 — Binary Triage & SDK Alignment** (triage complete, focused decompilation in progress).

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
| `decompile-all --name mbScript` | PASS | 5 procedures written, 0 failed (`pseudo/decompile-index.json`); entry `ev_7cd99ccc2639d6a0c4aab91f8cf6b1c8bd64971ccab2c1b77e8c0e8d8c157d7a` |
| Host PanVK loader/surface test | PASS | `host loader tests: PASS` (legacy renderer path, unaffected) |
| Ghidra toolchain health | PASS | `rea doctor`: ghidra 12.1.4, JDK 25.0.5, native decompiler and native authority all healthy |

## In progress

| Task | State |
| --- | --- |
| `decompile-all --name mbStatement` | Ghidra headless analysis running; output `pseudo_stmt/` |
| Trigger manager pseudocode | Layouts and entry points verified; decompilation queued after `mbStatement` |
| Opcode dispatcher identification | String anchors located (`Unrecognized opcode %d` @ `0x1405134d0`, `opcode %d out of range` @ `0x1405230d0`); `mbStatement::execute` / `executeWSE` pseudocode pending |

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
- `decompile-all` refuses a pre-created output directory; let the tool create it. Use `--resume`
  only when a previous export manifest exists.

## Next concrete task

1. When the `mbStatement` export finishes, read `mbStatement::readStream` / `parseOperand` /
   `execute` / `executeWSE` and `mbStatementBlock::readStream` / `analyze` / `execute`.
2. Confirm the 32-operand encoding and the `executeWSE` dispatch path against the WSESDK opcode
   table; locate the opcode dispatcher via the two diagnostic strings.
3. Decompile `mbTriggerManager::readStream` (`0x14023ea20`) and `execute` (`0x14023eee0`), then
   `mbSimpleTriggerManager` equivalents.
4. Add the first portable C++20 headers for `rglString`, `rglStream`, `mbStatement`,
   `mbStatementBlock`, and `mbScript`, with host unit tests.
5. Update `ROADMAP.md` and this file at the next milestone.
