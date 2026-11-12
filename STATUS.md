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
| Tracking documents | PASS | `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (15 classes, 1,644 symbols, 75 vtables) |
| Host PanVK loader/surface test | PASS | `host loader tests: PASS` (legacy renderer path, unaffected) |
| Ghidra toolchain health | PASS | `rea doctor`: ghidra 12.1.4, JDK 25.0.5, native decompiler and native authority all healthy |

## In progress

| Task | State |
| --- | --- |
| `decompile-all --name mbScript` | Ghidra headless analysis running (`pseudo_mbScript.log5` empty; output dir not yet materialised) |
| Pseudocode confirmation of script/trigger parse order | Blocked on the export above |
| Opcode dispatcher identification | String anchors located (`Unrecognized opcode %d` @ `0x1405134d0`, `opcode %d out of range` @ `0x1405230d0`); function not yet proven |

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

1. When the `mbScript` export finishes, read `mbScriptManager::readStream` (`0x14020b330`),
   `mbScript::readStream` (`0x14020b210`), `mbTriggerManager::readStream` (`0x14023ea20`), and
   `mbTriggerManager::execute` (`0x14023eee0`).
2. Cross-check every member access against the PDB offsets in `docs/SYMBOL_MAP.json`.
3. Locate the opcode dispatcher via xrefs to the two opcode diagnostics and map it to
   `WSELuaManager::operationMap`.
4. Add the first portable C++20 headers for `rglString`, `rglStream`, `mbStatementBlock`,
   `mbScript`, and the trigger managers, with host unit tests.
5. Update `ROADMAP.md` and this file at the next milestone.
