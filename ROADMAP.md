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
- [ ] Cross-reference public `WSESDK` header/operation files (`header_operations.py` / Lua bindings) against binary string references to locate the main opcode dispatcher, trigger manager, and RGL resource loaders.
  - Managers and script loaders located; opcode dispatcher anchors found at `0x1405134d0` and `0x1405230d0`; dispatcher pseudocode pending.
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
Finish Phase 1, Step 2: read the pending `mbScript` pseudocode, confirm the script/trigger parse order against the PDB layouts, and locate the opcode dispatcher via the two opcode-diagnostic strings.

## Current Position
- Phase 1, Step 1 is complete. Ghidra 12.1.4 + JDK 25.0.5 + Node 26.12.1 are installed under `C:\rea-tools` (the `%TEMP%` copies are wiped by server restarts). `rea doctor` reports the Ghidra provider healthy; REA still runs via CLI because it is not registered with OpenCode.
- Evidence: x64 image `c25b51a4…acdd2b`, 19,759 procedures, 7,157 strings; 23 DLLs / 542 imports; 14,877 PDB layouts, 47,947 symbols, 281 vtable runs (209 named, 75 `rgl*`/`mb*`/`WSE*`).
- Dependency corrections: Bullet Physics (393 types), FMOD Ex (`fmodex64.dll`, 53 delay imports), TinyXML, muParser, JSON, and stock Lua 5.1 (`lua5164.dll`, 103 imports). No Havok or LuaJIT evidence.
- `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json`, and `STATUS.md` are written; `decompile-all --name mbScript` is running in Ghidra headless.
- Host PanVK loader/surface tests pass; no Android NDK/SDK/device is available, so arm64 compilation remains blocked.
- Next: finish the focused decompilation, verify member offsets against pseudocode, then start portable C++20 headers for the script/trigger subsystem.
