# Recompilation plan (Phase 5, replaces the decompilation plan)

Decided with the user on 2026-11-18: rebuilding every gameplay function from decompiled
evidence (`docs/GAMEPLAY_PLAN.md`) would take too long. The game instead runs the **user's own
retail WSE2 code, translated from x86-64 to ARM64 ahead of time on the phone**. That
gives 1:1 behaviour and byte-compatible saves by construction. This document owns the design;
`ROADMAP.md` holds the checklist and `STATUS.md` holds progress.

## Decisions

| Question | Decision |
| --- | --- |
| Where translation runs | **On the phone**, once, at first launch (or when the exe changes), with a progress screen. The result is cached in app-private storage. |
| Compiler backend | **Embedded LLVM** (AArch64 target only) plus LLD as a library. x86-64 is lifted to LLVM IR and optimized, ARM64 is emitted, then linked into a cached `.so`. Expect +40–60 MB of APK. |
| Clean-room engine | **Hybrid.** Retail code runs the game. Our platform layer (SAF `Vfs`, Vulkan RHI, shader translation, `WseAudioMixer`, input and touch overlay, launcher) becomes the backend of the API shims. The reconstructed VM, loaders and savegame codecs become test oracles and fallbacks. |
| API layers | **Native shims allowed** for exactly the system DLLs the exe imports. Still banned: Wine, Box64, Dynarmic, QEMU, translating code during play (JIT), and bundling or committing anything retail. Lifted code is generated on the device and never leaves it. |
| Multiplayer | Out of scope (`steam_api64` is stubbed). |

## The target

Pinned to **WSE2 v1.1.5.5** `mb_warband_wse2_x64.exe`: 7,060,992 B, SHA-256
`d4e97d56…4e29709`, 14,618 `.pdata` entries. Any other build is refused with a clear message.
**The user's GOG install currently has the older 2026-10-14 build (6,735,360 B,
`c25b51a4…`): the phone install must carry the v1.1.5.5 exe, PDB and DLLs from the official
`WSE2.zip`.**

Import surface of v1.1.5.5 (546 functions in 23 DLLs). The C runtime is statically linked, so
there are no CRT DLLs to replace:

| Group | DLLs (functions) | Plan |
| --- | --- | --- |
| Shipped x64 DLLs | `lua5164` (103), `fmodex64` (54, delay-loaded) | **Translate them too**, from the user's install, like the exe. Their own imports are small (lua: 105 `KERNEL32`; FMOD: `WINMM` 22, `MSACM32` 6, `WSOCK32` 18, …) and go to the same shims. Fallback if FMOD output proves hard: an FMOD-API shim onto `WseAudioMixer`. |
| Core OS | `KERNEL32` (223), `USER32` (40), `ADVAPI32` (10), `GDI32`, `SHELL32`, `SHLWAPI`, `IMM32`, `ole32`, `OLEAUT32`, `PSAPI`, `dbghelp` (8) | Native shims: files go through the SAF `Vfs` (case-insensitive, `\` separators); threads, synchronization, time, memory and TLS are emulated; windows and messages are a fake `HWND` fed from SDL events into the exe's own `WndProc`. |
| Graphics | `d3d9` (1 + COM vtables), `d3dx9_43` (17) | Our own D3D9 subset over the Vulkan RHI (the P4 GPU path is a prerequisite). D3DX: DDS textures through `dds.h`, math, and effects: parse fx_2_0 (`mb_wse2_2a.fxo` / `2b.fxo`) and translate the SM2/3 shader bytecode to SPIR-V. |
| Input | `DINPUT8`, `XINPUT1_3` (3), `WINMM` (3) | SDL keyboard, mouse and gamepad; the P11 touch overlay produces keys and mouse input. |
| Network | `WS2_32` (37), `WINHTTP` (11), `WININET`, `urlmon`, `steam_api64` (10) | BSD sockets for local needs; HTTP and Steam are stubbed (single-player). |

## Runtime model

- **Guest image** mapped at its preferred base `0x140000000` (relocations applied if the
  phone's address space forbids it). Data sections are copied from the user's exe.
- **CPU state** per guest thread (16 GPRs, flags, XMM) is lifted per function. LLVM promotes
  registers to SSA inside a function, and flags are computed only where something reads them.
- **Calls:** direct calls become native calls. Indirect calls and vtable calls go through a
  guest-VA → host-function table. **An unknown target aborts loudly** with its address (no
  translation during play); the next translation pass adds it.
- **Memory ordering:** x86 is TSO and ARM64 is weak. Guest loads and stores to shared memory
  lower to `LDAPR`/`STLR`, `lock`-prefixed instructions to LSE atomics; provably thread-local
  (stack) accesses stay plain. Correct first, then cheaper.
- **Exceptions:** guest frames keep their Win64 layout on a guest stack, so a Win64 unwinder
  can walk them with the exe's own `.pdata` / `.xdata` and run MSVC C++ EH and SEH semantics.
  This is the largest open-ended piece (the 2026-11-11 dossier says the same).
- **Android loading:** the linked `.so` is written to app storage, made read-only (required
  for dynamically loaded code on Android 14+), and loaded with `dlopen`. R0 proves this on
  the user's phone first.

## Slices

| Slice | Done when |
| --- | --- |
| **R0 Feasibility spikes** (go/no-go) | On the user's Mali-G68 phone: (a) a `.so` generated and linked in-app loads and runs; (b) the image maps at `0x140000000`; (c) embedded LLVM + LLD build size and the translation speed for one large function are measured, and the whole-exe time is extrapolated; (d) one real retail function is translated from the user's exe and returns the same results as on the PC. If (a) or (c) fails, this plan is revisited before anything else. |
| **R1 Lifter** | PE + PDB + `.pdata` loader; function discovery (`.pdata`, PDB publics, call targets, jump tables, the no-`.pdata` leaf code); x86-64 decoding (Zydis, MIT); lifting to LLVM IR for integer, flags, SSE/SSE2 and string instructions. A coverage meter reports the share of functions that lift with no unsupported instruction. |
| **R2 Host differential rig** | The same lifter targets **x86-64 on the PC**, so translated and original code run side by side on the real CPU (as the 2026-11 flag rig did). Every function is checked on generated inputs. CI runs the lifter on synthetic test binaries only (no retail code), and an arm64 CI runner (`ubuntu-24.04-arm`) executes the ARM64 output. |
| **R3 Runtime core** | Guest memory, TEB/TLS, threads, the dispatch table, the import-thunk ABI bridge, memory-ordering lowering, Win64 unwinding and C++ EH. |
| **R4 OS shims** | All 546 imports listed with an owner; the startup path (`WinMain`, window, configuration, file I/O) runs to the main loop. |
| **R5 Graphics** | D3D9 subset and D3DX on the Vulkan RHI; fx_2_0 and SM2/3 → SPIR-V; the main menu renders. |
| **R6 Audio, input, Lua** | Translated `fmodex64` and `lua5164` over shims (or the FMOD-API fallback); DInput and XInput from SDL plus the touch overlay. |
| **R7 PC bring-up** | The whole exe, translated for x86-64, runs on Windows **with our shims only** (no `d3d9.dll`, no FMOD DLL loaded): main menu, campaign, a battle. Savegame oracle: retail saves load, and our saves load in retail. This is where most bugs get found, with a debugger, before the phone. |
| **R8 Phone bring-up** | Translation service and progress screen in the launcher, cache invalidated by the exe hash and translator version; Native from the main menu to a battle on Mali-G68. |
| **R9 Performance and hardening** | Profiling on the phone: cheaper ordering where provably safe, register passing between translated functions, hot-path tuning; a crash report listing missing targets and imports; several WSE2 mods (Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms) played end to end. |

## What carries over

- P-slices: the launcher (P10/P12), SAF `Vfs`, the Vulkan RHI and GPU path (P4), shader
  translation (P5), the audio mixer (P6), input and the touch overlay (P7/P11), and the
  performance hub (P8/P10) all become shim backends. P11.1 (UI aspect modes for our own UI
  renderer) matters less, because the retail renderer handles widescreen itself.
- The reconstructed engine: savegame codecs (round-trip oracle), loaders and the VM (behaviour
  oracles in tests), and every PDB layout in `docs/ARCHITECTURE.md` / `SYMBOL_MAP.json`
  (needed to write shims that touch engine structures).
- The 2026-11-11 recompilation proofs (`analysis/`, `proof/`, `runtime/`): flag model, TEB/TLS,
  import thunks and the differential approach, as reference. Their figures are for the older
  build.

## Risks

| Risk | Plan |
| --- | --- |
| Android refuses to run generated code | R0 (a) first; fallback: anonymous executable memory loaded by our own ELF loader. |
| Translation time and APK size on the phone | R0 (c) measures it; parallel translation, `-O1` for cold functions, a cache keyed by the exe hash. |
| C++ exceptions and SEH | R3, tested on the PC host rig before the phone. |
| D3DX effect bytecode | R5, tested offline against the two `.fxo` files. |
| Weak-memory bugs that single-threaded tests miss | Conservative ordering by default; multi-threaded stress runs on the phone. |
| Wrong exe build | Hash pin; the launcher reports a mismatch and what to install. |
