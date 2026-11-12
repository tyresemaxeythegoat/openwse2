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
  - 19,759 procedures, 7,157 strings, 23 DLLs / 542 imports; 14,877 PDB layouts, 47,947 symbols, 209 named vtables.
- [x] Cross-reference public `WSESDK` header/operation files (`header_operations.py` / Lua bindings) against binary string references to locate the main opcode dispatcher, trigger manager, and RGL resource loaders.
  - Managers and script loaders located; `mbStatement::executeWSE` (`0x1401e0cf0`) mapped to all 433 SDK operations in the analyzed 2026-10-14 build (extended range 2800–5202, 424 switch cases, 9 equality-tested cases). WSE2 v1.1.5.5 adds seven operations (3911, 4517–4522) without renumbering; the retarget decision is tracked in `STATUS.md`.
- [x] Write `docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json` before writing engine code.
  - Additional `STATUS.md` tracks phase, gates, and blockers.

### Phase 2: Core Data Structures & Module System VM
- [ ] Reconstruct core RGL types (strings, vectors, matrices, `.brf` mesh/texture loaders, `.txt` module parsers) in portable C++20.
- [ ] Reconstruct the Warband Module System script VM, all WSE2 extended opcodes, and ARM64-compatible Lua 5.1 integration (the binary imports `lua5164.dll`, not LuaJIT).
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
Phase 1, Step 2 is complete: the 433-operation `executeWSE` map is extracted and three handler groups
(2800–2809, the floating-point load/store/compare/arithmetic group, and the transcendental/rounding
group) are host-tested. Continue Phase 2 by transcribing the remaining `executeWSE` handler groups and
wiring reconstructed statement blocks into the trigger runtime.

## Current Position
- Phase 1, Step 1 is complete. Ghidra 12.1.4 + JDK 25.0.5 + Node 26.12.1 are installed under `C:\rea-tools` (the `%TEMP%` copies are wiped by server restarts). `rea doctor` reports the Ghidra provider healthy; REA still runs via CLI because it is not registered with OpenCode.
- Evidence: x64 image `c25b51a4…acdd2b`, 19,759 procedures, 7,157 strings; 23 DLLs / 542 imports; 14,877 PDB layouts, 47,947 symbols, 281 vtable runs (209 named, 75 `rgl*`/`mb*`/`WSE*`).
- Dependency corrections: Bullet Physics (393 types), FMOD Ex (`fmodex64.dll`, 53 delay imports), TinyXML, muParser, JSON, and stock Lua 5.1 (`lua5164.dll`, 103 imports). No Havok or LuaJIT evidence.
- `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (25 classes), and `STATUS.md` are written.
- `decompile-all --name mbScript` completed: 5 procedures, 0 failures. Pseudocode confirms `mbScriptManager::readStream`, `mbScript::readStream`, and `mapScripts` (64-entry name table at `0x1405c7ad0`).
- `decompile-all --name mbStatement` completed: 14 procedures, 0 failures. Confirmed the statement record (`uint32` opcode, `int32` count, 32 `int64` operands, `m_endStatement`), the 7-bit operand tag scheme (`0` immediate, `1` global, `2` global vector, `0x11` local), `analyze` control-flow resolution, the 2,738-entry native jump table at `0x1401de220` for opcodes 30–2767, and `executeWSE` for opcodes ≥ 2768.
- Address-range export completed: `mbStatement::parseOperand` and `returnValue` recovered (3/3 written). Operand 0 is the destination lvalue; `returnValue` writes through `g_globalVariables`, `mbSlots::set`, or `m_locals`, and rejects other tags with `"Illegal lvalue in statement"`.
- `decompile-all --name Trigger` completed: 52 procedures, 0 failures. Confirmed both trigger manager loaders, all trigger layouts, the three-token header, the `"Game"`/`"Simple"` id prefixes, `g_timersTotal` timer seeding, and the savegame order.
- `decompile-all --name rglTextStream` completed: 18 procedures, 0 failures. Vtable `0x14051c0b0` is `rglInputTextFileStream`; `0x14051c2b0` is `rglOutputTextFileStream`. Module files are whitespace-delimited decimal text read through the inherited `rglTextStream` slots (`+0x58` float, `+0x60` bool, `+0x70` int64, `+0x78`/`+0x80` 32-bit, `+0x90` raw string).
- Trigger runtime: `mbTriggerManager::execute` inlines the state machine; the integer argument is an event key. Non-negative keys run interval triggers; negative keys match the rounded check interval (confirmed `-112`, `-105`/`-106`, `-26`/`-56`, `-27`, `-28`, `-110`/`-109`, `-102`, `-113`).
- `decompile-all --name rglBinaryStream` completed: 9 procedures, 0 failures, plus a 2-procedure address-range export for the readers. Savegame values are raw little-endian 8/4/1-byte copies with no header; strings are `int32` length + bytes. Trigger savegame order is reconstructed, including the count-exceeded message-box path that still consumes surplus entries.
- Portable `engine/` now covers the token reader (including booleans), tagged operands, `mbSlots`, statements, script managers, triggers, the event-key trigger/simple-trigger state machines, the binary savegame stream, and trigger savegame read/write; `engine/build_host.ps1` reports `operand tests: PASS`, `trigger tests: PASS`, `savegame tests: PASS`, `script tests: PASS`, `wse tests: PASS`, `engine host tests: PASS`.
- `executeWSE` opcode map complete: `tools/extract_wse_opcodes.py` regenerates `docs/wse_opcodes.json`, `docs/WSE_OPCODES.md`, and `engine/include/wse/vm/wse_opcodes.h` from the 9,245-line pseudocode dump plus `header_operations_addon.py`. All 433 SDK operations in 2800–5202 have handlers; there are no unknown or unhandled in-range opcodes.
- Portable `engine/include/wse/vm/execute_wse.h` applies the `0x0FFFFFFF` opcode mask and implements 44 opcodes: the 2800–2809 group (arithmetic/logical right shifts, left shift, xor, complement; shift counts masked with `& 0x3F`) and the floating-point group 4400/4405–4438 (128-register bank, fixed-point scale 100, `rglRound64`, comparisons, arithmetic, min/max/clamp, sqrt/abs/ceil/floor/exp/pow/log/trig/atan2). The other 389 mapped opcodes report `WseDispatch::unimplemented` until transcribed.
- WSE2 v1.1.5.5 (2026-11-12) was published after the analyzed build. Its SDK keeps all existing opcode numbers and adds seven operations (3911 and 4517–4522); the official zip is verified at SHA-256 `c45197e8…276e28b1`. Retargeting the Ghidra baseline is a pending decision because it would shift every cited address.
- The script reader parsed the full retail 3,467,512-byte `scripts.txt` sample: 1,755 scripts, every token consumed, 52 of 64 static names mapped (the rest are WSE-only scripts absent from that module). All 64 names are transcribed from the table at `0x1405c7ad0`; duplicate ids resolve to the first script in file order.
- Host PanVK loader/surface tests pass; no Android NDK/SDK/device is available, so arm64 compilation remains blocked.
- Next: transcribe the remaining `executeWSE` handler groups and wire statement-block execution into triggers.
