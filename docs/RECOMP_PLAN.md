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

Accepts **any WSE2 x64 build whose shipped PDB matches** (`mb_warband_wse2_x64.exe` +
`mb_warband_wse2_x64.pdb` RSDS GUID match; functions are resolved by symbol name rather than
pinned addresses). **WSE2 v1.1.5.6** (7,066,112 B, SHA-256 `fbe9808c…4164898`, PDB age 456,
14,644 `.pdata` entries, PE timestamp 2026-11-15 — on both the user's GOG PC install and phone
install) is the verified baseline. Earlier R0 builds pinned **WSE2 v1.1.5.5** (7,060,992 B,
SHA-256 `d4e97d56…4e29709`, PDB age 449, 14,618 `.pdata` entries; the 2026-11-19 R0 phone run
happened under that pin and refused v1.1.5.6).

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

## R0 implementation (2026-11-18)

Decided with the user: all four spikes in one batch, LLVM built from source in CI, results
read on the phone from Advanced > "R0 spikes" (Copy / Share), using the v1.1.5.5 exe already
in the phone's install.

| Piece | Where |
| --- | --- |
| x86-64 decoder subset, reference interpreter, PE reader, R0 (d) vectors, synthetic test code | `engine/include/wse/recomp/` (header-only, host suite `recomp`) |
| LLVM 18 lifter, optimizer, object emission, embedded LLD, R0 spike runner | `engine/src/recomp/` (`wse_recomp`, only with `WSE_ENABLE_LLVM=ON`) |
| Android library and entry | `libwse_r0.so` (`platform/android/r0_jni.cpp`), loaded only by `R0Spikes.kt` |
| LLVM for the phone | CI job `llvm-android`: tag `llvmorg-18.1.8` (commit `3b5b5c1e…`, checked), NDK `30.0.16248370`, `MinSizeRel`, AArch64 target only, static, no RTTI; cached |
| Translator test on a PC | CI job `recomp-host` (Ubuntu `llvm-18-dev`, synthetic code only) |
| PC reference for (d) | `tools/r0_pc_reference.cpp`, Windows, local only (calls the user's retail code) |

How each spike is answered on the phone:

- **(a)** LLVM emits a small probe function, LLD links it, and the report tries three loaders
  in order: `dlopen` of the file made read-only, `dlopen` from a `memfd`
  (`android_dlopen_ext`), and our own ELF loader into anonymous memory (the Risks fallback).
  Later steps use the first loader that worked. Android may refuse executable mappings of
  app-data files for apps targeting API 29+, which is why all three are reported.
- **(b)** `mmap(0x140000000, SizeOfImage, MAP_FIXED_NOREPLACE)`, then the exe's sections are <!-- citation-check: ignore-line -->
  copied in.
- **(c)** The APK cost of `libwse_r0.so` is printed by the CI APK check. On the phone: a
  42 KB synthetic function (2,000 blocks) is lifted, optimized at O2 and O1, compiled, linked
  and checked against the interpreter; every retail function the R0 subset decodes is
  translated as one batch; the whole `.text` time is extrapolated from both rates.
- **(d)** `rglString::indexOf` is decoded from the mapped image, translated at O2, and run on
  the 512 cases; the report compares its hash with the PC's `0x7aeac64f5cbff537`, the
  interpreter and the model.

R0 deviation: translated loads and stores are plain (no `LDAPR`/`STLR`); the memory-ordering
lowering lands in R3. A run that crashes inside generated code leaves `r0/report.txt`, which
the next run shows.

## R1 design (2026-11-19)

Decided with the user: Zydis 4.1.0 decodes (MIT, amalgamated `Zydis.c`/`Zydis.h`, archive
SHA-256 `aa9b82be3a37a2998bd8e16cf583bbf2b6c3d80e97dc20504169dc32ca1ced59`, pinned in CMake);
the reference is a real CPU, not a second implementation; all of R1 lands as one batch.
Measured on v1.1.5.6 (Capstone, local): 1,181,537 instructions in 14,644 `.pdata` functions,
243 mnemonics. Functions containing: integer 14,514; direct calls 9,779; `rep`/string 4,345;
indirect calls 3,396; SSE 3,284 (mostly scalar float: `movss` 39,542, `mulss` 18,649); system
(`int3`, `cpuid`, …) 819; indirect `jmp` 363; `lock` 201; x87 18 (48 instructions); AVX none.

**CPU state** (`wse/recomp/cpu_state.h`): 16 GPRs, `rip`, flags CF PF AF ZF SF OF DF as bytes,
16 XMM registers (16-byte aligned), MXCSR. R0's offsets stay valid; new fields follow them.

**Function ABI.** A translated function is `void fn(CpuState*)` and runs with guest memory equal
to host memory (the image is mapped at its base). Guest calls keep the Win64 stack exactly: a
direct `call T` pushes the return VA on the guest stack and calls `fn_T(state)`; on return the
caller checks `state->rip` equals that return VA (anything else calls the `bad_return` hook).
Every call out of translated code goes through the `RuntimeHooks` table that `CpuState::hooks`
points at (`wse/recomp/cpu_state.h`), so a translated module imports nothing; the link uses
`-z defs` to enforce that. Indirect calls and `jmp`s to another function call `dispatch(state,
va)`, which looks the VA up in the `wse_fn_table` each module exports (defined functions only);
an unknown VA is a fault that aborts loudly with the address (no translation during play). Calls
through the import table call `import_call(state, slot_va)` (the R4 shims plug in there).
`gs:`/`fs:` operands ask `segment_base` (R3). A 64-bit `div`/`idiv` whose `rdx` is more than
`rax`'s extension calls `div128`, because LLVM lowers a 128-bit division to a compiler-rt call.
A `jmp` to another function's entry is a tail call. The host side is `wse/recomp/runtime.h`
(`Runtime`: the table, the hooks, and faults reported to a handler).

**Function discovery.** Entries: `.pdata` begins (merging chained-unwind fragments into their
parent), PDB publics in `.text`, and every direct call target. Bodies are found by following
control flow from the entry (not by the `.pdata` range), so code shared between fragments is
reached. Indirect `jmp`s inside a function are resolved as MSVC jump tables (bounded by the
preceding `cmp`/`ja`, or by an `and idx, 2^k - 1` mask), in MSVC's shape (image-relative
entries) and clang's (entries relative to the table); an unresolved one aborts at run time with
its VA.

**Lifting.** Integer (all ALU, shifts/rotates, `mul`/`imul`/`div`/`idiv`, `bt*`, `setcc`,
`cmovcc`, `bswap`, `bsf`/`bsr`/`lzcnt`/`tzcnt`/`popcnt`, `cbw`…`cqo`, `xchg`, `xadd`,
`cmpxchg`), `lock` forms as LLVM atomics, SSE/SSE2/SSE3/SSSE3/SSE4.1 scalar and packed
(float, double, integer, conversions honouring MXCSR rounding, shuffles), and `rep`/string
instructions as loops honouring DF. x87, AVX and privileged instructions are reported as
unsupported. Memory ordering stays plain until R3.

R1 deviations (each also noted next to its code): a `lock` operand is assumed naturally aligned
(AArch64 atomics need it; x86 would bus-lock a misaligned one); `cvtsd2ss`/`cvtpd2ps` round to
nearest whatever MXCSR.RC says; with two NaN inputs the NaN that propagates may differ from x86
(LLVM treats `fadd` and friends as commutative); string instructions with a `gs:`/`fs:` override
and 16-bit addressing are unsupported.

**Coverage meter.** Per build: how many functions decode, lift and verify, with the top
unsupported mnemonics. It runs with the phone's R0 report and in CI on the synthetic corpus.

**Oracle.** CI (x86-64 Linux): a corpus of C functions (ours: `engine/tests/recomp_corpus/`
`int_corpus.c` without SSE, `string_corpus.c` with inline-asm string instructions, `sse_corpus.c`
at `-msse4.1` with the vectorizers on) compiled by clang for Win64
(`--target=x86_64-pc-windows-msvc`, freestanding) and linked by lld-link into one PE at
0x140000000; the test maps it at its base, runs each export natively through an `ms_abi` pointer
and translated for x86-64, 200 generated inputs each, and compares `rax`, the memory buffer, the
callee-saved registers (`rbx rbp rsi rdi r12-r15`, `xmm6-xmm15`), `rsp` and DF. Functions that are
reached only through a pointer table are exported, as retail code reaches them through PDB
publics. Retail: this PC runs chosen retail
functions natively and records result hashes (as `tools/r0_pc_reference.cpp` does); the phone
runs their translations and compares.

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
