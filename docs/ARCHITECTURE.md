# WSE2 Architecture (Reconstructed)

Status: Phase 1 — binary triage complete; decompilation of the script/trigger subsystem in progress.
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

The two opcode diagnostics are the anchors for locating the dispatcher; the exact dispatch function
is **open** until the current `decompile-all --name mbScript` export completes. The manager flow above
is proven by symbol names, layouts, and global instances; byte-level parse order still needs
pseudocode confirmation.

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

1. Pseudocode for `mbScriptManager::readStream`, `mbTriggerManager::readStream`,
   `mbSimpleTriggerManager::readStream`, and `mbGame::readTriggers` to confirm token order and
   opcode encoding against WSESDK.
2. Locate `gameOperation` table construction in `WSELuaManager` and map opcode IDs to handlers.
3. Confirm `rglConfig`/`module.ini` key names and defaults from string xrefs.
4. Confirm renderer resource formats (`.brf`, `.dds`) from `rglMesh`/`rglMaterial` loaders before
   writing asset code.
5. Validate every reconstructed offset against decompiled member accesses before committing C++.

## 9. Legal guardrail

No game assets, retail binaries, PDBs, or derived disassembly are checked into the repository.
The Android app is BYOA: it asks the user to select a legally owned Warband installation and reads
`Modules/` from that location. No DRM or license check is bypassed.
