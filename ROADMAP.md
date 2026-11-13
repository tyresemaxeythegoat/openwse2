# ROADMAP — Clean-Room WSE2 Android Port

Active plan for the WSE2 interoperability port. It supersedes the static-recompilation approach described in `README.md`, `analysis/`, `proof/`, and `runtime/`; those artifacts remain as CPU/ABI reference only.

See `AGENTS.md` for role, repository location, toolchain commands, and environment quirks.

## Strict Legal & Technical Guardrails
1. **Evidence-backed decompilation only.** Never hallucinate memory offsets, struct sizes, or function behavior. Every reconstructed class, vtable, and function must cite evidence queried via REA/Ghidra (symbol address, RTTI name, string cross-reference, or decompiled pseudocode).
2. **Bring Your Own Assets (BYOA).** This is an engine reimplementation for interoperability. Do not bundle proprietary game assets and do not implement DRM circumvention. The Android app must prompt the user to select their own legally owned Mount & Blade: Warband PC game directory and `Modules/` folders.
3. **Incremental compilation gate.** Never move to a new subsystem until the current C++ module compiles cleanly under both host native tests and the Android NDK (`clang++`, target `aarch64-linux-android29`, `-Wall -Werror`).

## Workspace Tracking Files
Maintain and update these after every milestone:
- `docs/ARCHITECTURE.md` — subsystem map, RTTI class hierarchy, external dependencies (Lua 5.1, FMOD Ex/OpenAL, Bullet Physics).
- `docs/SYMBOL_MAP.json` — verified mapping of binary addresses to reconstructed C++ functions and structs.
- `STATUS.md` — current phase, passing/failing CMake targets, next concrete task.

## Current Target
- Primary analysis binary: `mb_warband_wse2_x64.exe` (PDB-backed; image base `0x140000000`). The plan also names the 32-bit `mb_warband_wse2.exe`, which is present in `WSE2.zip`; confirm before switching.
- Reference device in existing docs: OPPO Reno 11, Mali-G68 (PanVK). No device is currently attached.

## Phased Execution Plan

### Phase 1: Binary Triage & SDK Alignment
- [x] Load the target binary through REA/Ghidra and extract imports, strings, RTTI descriptors, and global tables.
  - 20,276 procedures, 7,448 strings, 23 DLLs / 546 imports (`ev_a4af0bca…`); 15,363 PDB layouts, 48,658 symbols, 207 destructor-named vtables (73 `rgl*`/`mb*`/`WSE*`); `docs/SYMBOL_MAP.json` indexes 29 classes / 1,782 symbols / 73 vtables.
- [x] Cross-reference public `WSESDK` header/operation files (`header_operations.py` / Lua bindings) against binary string references to locate the main opcode dispatcher, trigger manager, and RGL resource loaders.
  - Managers and script loaders located; `mbStatement::executeWSE` (`0x1401e4dc0`) mapped to all 440 SDK operations in WSE2 v1.1.5.5 (extended range 2800–5202, 431 switch cases, 9 equality-tested cases), including the seven operations v1.1.5.5 added (3911, 4517–4522).
- [x] Write `docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json` before writing engine code.
  - Additional `STATUS.md` tracks phase, gates, and blockers.

### Phase 2: Core Data Structures & Module System VM
- [ ] Reconstruct core RGL types (strings, vectors, matrices, `.brf` mesh/texture loaders, `.txt` module parsers) in portable C++20.
- [ ] Reconstruct the Warband Module System script VM (`block_vm.h` core VM + trigger/script runtime complete), all WSE2 extended opcodes (93/440 implemented), and ARM64-compatible Lua 5.1 integration (the binary imports `lua5164.dll`, not LuaJIT).
- [ ] Add headless unit tests proving the reconstructed parser and VM load and execute standard `Native` and WSE2 module scripts.

### Phase 3: Cross-Platform Replacement (Win32/DX9 -> SDL2/GLES3.2)
- [ ] Add a root `CMakeLists.txt` for desktop host builds and Android Gradle/NDK `arm64-v8a` builds with SDL2.
- [ ] Replace Win32 filesystem, threading, memory management, and timing with C++20 (`std::filesystem`, `std::thread`) and SDL2 equivalents.
- [ ] Replace DirectX 9 rendering with a modular OpenGL ES 3.2 / Vulkan backend; translate/cross-compile WSE2 shaders (SPIRV-Cross / HLSLcc).
- [ ] Replace Windows audio hooks with OpenAL Soft or Oboe/SDL_mixer for low-latency Android audio.

### Phase 4: Native Android Features
- [ ] **Asset & Mod Loader UI:** SAF/folder-permission launch screen to select local `Modules/` directories and switch mods (equivalent to `wse2_launcher.exe`).
- [ ] **Dynamic touch controls:** context-sensitive overlay (virtual movement stick, 4-direction combat swipe/block zones, battle command radial menu) that hides during menu/inventory screens or when a Bluetooth controller is connected.
- [ ] **Sensor & performance hooks:** optional gyro aiming when bows/crossbows are drawn, pinch-to-zoom on the campaign map, Android thermal-throttling awareness, and configurable frame-rate caps (60/90/120 Hz).

## Immediate First Action
Phase 1, Step 2 is complete: the 440-operation `executeWSE` map is extracted from WSE2 v1.1.5.5 and
93 of 440 opcodes across seven handler groups
(2800–2809, 3000–3002, 3014/3016/3017/3035/3600, string ops 4200–4224/4233–4234/4236/4238/4240–4244,
options flags 4300–4305, `fld_str` 4401, the floating-point load/store/compare/arithmetic group, and
the transcendental/rounding group) are host-tested.
Continue Phase 2 by transcribing the remaining `executeWSE` handler groups (game-state string/pos
queries such as 4225–4239, 4402–4404, 4100+, etc.) and wiring reconstructed statement blocks into
the trigger runtime.

## Current Position
- Phase 1, Step 1 is complete. Ghidra 12.1.4 + JDK 25.0.5 + Node 26.12.1 are installed under `C:\rea-tools` (the `%TEMP%` copies are wiped by server restarts). `rea doctor` reports the Ghidra provider healthy; REA still runs via CLI because it is not registered with OpenCode.
- Evidence baseline: official WSE2 v1.1.5.5 x64 image `d4e97d56…4e29709` (7,060,992 B, file version 1.1.5.5), PDB `fccf5e8d-4952-4a53-aa13-ac8b7ad38cfc` age 449; 20,268 procedures; 15,363 PDB layouts, 48,658 symbols, 279 vtable runs (207 named, 73 `rgl*`/`mb*`/`WSE*`). The 2026-10-14 build (`c25b51a4…acdd2b`) is superseded.
- Dependency corrections: Bullet Physics (393 types), FMOD Ex (`fmodex64.dll`, 54 delay imports), TinyXML, muParser, JSON, and stock Lua 5.1 (`lua5164.dll`, 103 imports). No Havok or LuaJIT evidence.
- `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (25 classes), and `STATUS.md` are written.
- v1.1.5.5 address-range export of `executeWSE` (`0x1401e4dc0`) completed: 1 written, 0 failed, evidence `ev_2aaa96a8…5cf0f9`, 8,844-line dump; 440/440 in-range SDK operations mapped (431 switch + 9 equality). The PDB confirms `mbScriptManager::readStream`, `mbScript::readStream`, and `mapScripts` (65-entry name table at `0x140615ad0`).
- `decompile-all --name mbStatement` completed: 14 procedures, 0 failures. Confirmed the statement record (`uint32` opcode, `int32` count, 32 `int64` operands, `m_endStatement`), the 7-bit operand tag scheme (`0` immediate, `1` global, `2` global vector, `0x11` local), `analyze` control-flow resolution, the 2,738-entry native jump table at `0x1401e22f0` for opcodes 30–2767, and `executeWSE` for opcodes ≥ 2768.
- Address-range export completed: `mbStatement::parseOperand` and `returnValue` recovered (3/3 written). Operand 0 is the destination lvalue; `returnValue` writes through `g_globalVariables`, `mbSlots::set`, or `m_locals`, and rejects other tags with `"Illegal lvalue in statement"`.
- `decompile-all --name Trigger` completed: 52 procedures, 0 failures. Confirmed both trigger manager loaders, all trigger layouts, the three-token header, the `"Game"`/`"Simple"` id prefixes, `g_timersTotal` timer seeding, and the savegame order.
- `decompile-all --name rglTextStream` completed: 18 procedures, 0 failures. Vtable `0x1405651e0` is `rglInputTextFileStream`; `0x1405653e0` is `rglOutputTextFileStream`. Module files are whitespace-delimited decimal text read through the inherited `rglTextStream` slots (`+0x58` float, `+0x60` bool, `+0x70` int64, `+0x78`/`+0x80` 32-bit, `+0x90` raw string).
- Trigger runtime: `mbTriggerManager::execute` inlines the state machine; the integer argument is an event key. Non-negative keys run interval triggers; negative keys match the rounded check interval (confirmed `-112`, `-105`/`-106`, `-26`/`-56`, `-27`, `-28`, `-110`/`-109`, `-102`, `-113`).
- `decompile-all --name rglBinaryStream` completed: 9 procedures, 0 failures, plus a 2-procedure address-range export for the readers. Savegame values are raw little-endian 8/4/1-byte copies with no header; strings are `int32` length + bytes. Trigger savegame order is reconstructed, including the count-exceeded message-box path that still consumes surplus entries.
- Portable `engine/` now covers the token reader (including booleans), tagged operands, `mbSlots`, statements, script managers, triggers, the event-key trigger/simple-trigger state machines, the binary savegame stream, and trigger savegame read/write; `engine/build_host.ps1` reports `operand tests: PASS`, `trigger tests: PASS`, `savegame tests: PASS`, `script tests: PASS`, `wse tests: PASS`, `engine host tests: PASS`.
- `executeWSE` opcode map: `tools/extract_wse_opcodes.py` regenerates `docs/wse_opcodes.json`, `docs/WSE_OPCODES.md`, and `engine/include/wse/vm/wse_opcodes.h` from the Ghidra pseudocode plus `header_operations_addon.py`; the VA is read from the pseudocode header, so a retarget cannot leave stale citations. The v1.1.5.5 SDK has 440 in-range operations (2800–5202).
- Portable `engine/include/wse/vm/execute_wse.h` applies the `0x0FFFFFFF` opcode mask and implements 93 of 440 opcodes: 2800–2809 (arithmetic/logical right shifts, left shift, xor, complement; shift counts masked with `& 0x3F`), 3000–3002 (`register_get`/`register_set`/`store_wse_version`), 3014/3016/3017/3035/3600 (script-context queries), string operations 4200–4224, 4233–4234, 4236, 4238, and 4240–4244 (writable `WseStringBanks::registers` tag-0 bank, comparisons/predicates/search/num/split/sort/store ops, RFC 1321 MD5, float formatting, sanitize, integer predicate, RCON no-op, ECMAScript `std::regex` match/search/get_matches/replace, URL unescape), options flags 4300–4305 (`WseVmState::options`), and the floating-point group 4400–4401/4405–4438 (128-register bank, fixed-point scale 100, `fld_str`, `rglRound64`, comparisons, arithmetic, min/max/clamp, sqrt/abs/ceil/floor/exp/pow/log/trig/atan2). The other 347 mapped opcodes report `WseDispatch::unimplemented` until transcribed.
- WSE2 v1.1.5.5 (2026-11-12) is now the evidence baseline (official `WSE2.zip` SHA-256 `c45197e8…276e28b1`). It keeps every existing opcode number and adds seven operations (3911 and 4517–4522) plus one mapped script callback, `wse_camp_button_clicked` (table entry 64, `m_mappedScripts[65]`).
- The script reader parsed the full retail 3,467,512-byte `scripts.txt` sample: 1,755 scripts, every token consumed, 52 of 64 static names mapped (the rest are WSE-only scripts absent from that module). All 65 v1.1.5.5 names are transcribed from the table at `0x140615ad0`; duplicate ids resolve to the first script in file order.
- Host PanVK loader/surface tests pass; no Android NDK/SDK/device is available, so arm64 compilation remains blocked.
- `block_vm.h` reconstructs the Warband statement-block interpreter (`mbStatementBlock::analyze`/`execute`/`startLoop`/`endLoop`/`canLoop`, core comparisons/math/fixed-point ops `30–33`/`2100–2133`, `call_script` up to depth 255, `store_script_param*`, `this_or_next`/`neg`) and wires VM execution into `execute_script`, `execute_mapped_script`, `execute_trigger*`, and `execute_simple_trigger*` (validated by all 6 host C++20 suites); `AGENTS.md` is added and `docs/SYMBOL_MAP.json` expands to 29 classes / 1,782 symbols / 73 vtables.
- Next: transcribe the remaining `executeWSE` handler groups beyond 93/440 and run the `aarch64-linux-android29` NDK compile gate once an NDK is available.
