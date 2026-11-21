# Recompilation plan (Phase 5, replaces the decompilation plan)

Decided with the user on 2026-11-18: rebuilding every gameplay function from decompiled
evidence (`docs/GAMEPLAY_PLAN.md`) would take too long. The game instead runs the **user's own
retail WSE2 code, translated from x86-64 to ARM64 ahead of time on the phone**. That
gives 1:1 behaviour and byte-compatible saves by construction. This document owns the design;
`ROADMAP.md` holds the checklist and `STATUS.md` holds progress.

## Decisions

| Question | Decision |
| --- | --- |
| Where translation runs | **On the phone** (fixed so oversized batches do not OOM-stall), once at first launch or when the exe/translator changes, **with PC preparation as an optional fast path** (standalone prebuilt CLI zip for players, no WSL required; decided with the user, 2026-11-21). |
| Compiler backend | **Embedded LLVM** (AArch64 target only) plus LLD as a library. x86-64 is lifted to LLVM IR and optimized, ARM64 is emitted, then linked into cached `.so` batches. |
| Clean-room engine | **Hybrid.** Retail code runs the game. Our platform layer (SAF `Vfs`, Vulkan RHI, shader translation, `WseAudioMixer`, input and touch overlay, launcher) becomes the backend of the API shims. The reconstructed VM, loaders and savegame codecs become test oracles and fallbacks. |
| API layers | **Native shims allowed** for exactly the system DLLs the exe imports. Still banned: Wine, Box64, Dynarmic, QEMU, translating code during play (JIT), and bundling or committing anything retail. Lifted code is generated on the device (or on the player's own PC for their own phone) and never distributed. |
| Target devices & v1 bar | **`arm64-v8a` Android 10+ phones with ARMv8.2-A (LSE + RCpc), Vulkan and at least 6 GB RAM, tiered by chip and GPU, not by RAM.** Reference phone: the user's **OPPO Reno 11, 12 GB RAM, Mali-G68** (a mid-range GPU; PanVK and System Vulkan); also mid-range Adreno/Turnip. Out of scope: pre-ARMv8.2-A CPUs or under 6 GB RAM. **v1 playable bar:** `Native` end-to-end (main menu, campaign map, scenes, battles, saves); heavy WSE2 mods follow in R9 (decided with the user, 2026-11-21). |
| Sustained performance target | **Mid-range GPU tier (the reference phone, Mali-G68 class):** 30 FPS sustained for 30+ minutes in 100-troop battles at ~720p / 85% render scale, and 60 FPS in menus/map. **Flagship GPU tier (Snapdragon 8 Gen 2+ / Dimensity 9000+ class):** playable **1,000+ troop WSE2 battles** at ≥ 30 FPS sustained. **Every supported phone:** 1,000+ troop battles load and run with no OOM and no buffer/descriptor cap failures; on the reference phone their frame rate is measured and recorded, not gated (decided with the user, 2026-11-21; tiers by chip after the user's 12 GB Mali-G68 phone fit neither RAM tier). |
| Audio (`fmodex64`) | **Shim the 54 delay-loaded FMOD Ex exports directly onto `WseAudioMixer` + SDL3 audio** (P6) rather than translating `fmodex64.dll` (decided with the user, 2026-11-21). |
| Multiplayer | **Planned for R12 (post-v1):** offline stubs through R8–R11 so single-player ships cleanly; R12 implements real `WS2_32` BSD sockets and Android TLS `WINHTTP`/`WININET` for Wi-Fi LAN, direct-IP hosts, and non-Steam WSE2 community servers (`steam_api64` stays stubbed, no DRM bypass; decided with the user, 2026-11-22). |
| Distribution | **GitHub Releases only** (no Google Play Store): signed release APKs with a dedicated private release keystore (never `android/debug.keystore`), plus a standalone prebuilt Windows/Linux PC preparation CLI zip (decided with the user, 2026-11-21 / 2026-11-22). |

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
| Shipped x64 DLLs | `lua5164` (103), `fmodex64` (54, delay-loaded) | **Translate `lua5164.dll`** from the user's install (its only imports are 105 `KERNEL32`). **Shim `fmodex64`** directly onto P6's `WseAudioMixer` + SDL3 audio stream (decided with the user, 2026-11-21; avoids translating FMOD and shimming `WINMM`/`MSACM32`/`WSOCK32`). |
| Core OS | `KERNEL32` (223), `USER32` (40), `ADVAPI32` (10), `GDI32`, `SHELL32`, `SHLWAPI`, `IMM32`, `ole32`, `OLEAUT32`, `PSAPI`, `dbghelp` (8) | Native shims: files go through the SAF `Vfs` (case-insensitive, `\` separators) with a permanent app-private writable layer (`filesDir/wse2/user/`); threads, synchronization, time, memory and TLS are emulated; windows and messages are a fake `HWND` fed from SDL events (including soft-keyboard `WM_CHAR`/`WM_KEYDOWN`) into the exe's own `WndProc`. |
| Graphics | `d3d9` (1 + COM vtables), `d3dx9_43` (17) | Our own D3D9 subset over the Vulkan RHI (P4 GPU path). Two-step shaders (decided with the user, 2026-11-21): R5a maps `ID3DXEffect` technique/parameter names onto P5's `GLShaders` → SPIR-V pipeline; R5b adds `fx_2_0` / SM2–3 bytecode → SPIR-V for custom mod `.fxo` effects. Textures use native Vulkan BC when supported, else one-time cached ETC2/ASTC transcoding (RGBA8 + mip-cap as temporary R5a bring-up fallback). |
| Input | `DINPUT8`, `XINPUT1_3` (3), `WINMM` (3) | SDL keyboard, mouse and gamepad; context-gated touch overlay drawn in Vulkan inside `Present()` (`docs/PLATFORM_PLAN.md` "P14"). |
| Network | `WS2_32` (37), `WINHTTP` (11), `WININET`, `urlmon`, `steam_api64` (10) | Offline stubs through v1 (R4–R11); R12 wires `WS2_32` onto POSIX/BSD sockets and `WINHTTP`/`WININET` onto Android TLS for LAN, direct-IP, and non-Steam WSE2 community servers (`steam_api64` stays stubbed; decided with the user, 2026-11-22). |

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
| **R4 OS shims** | All 546 imports listed with an owner; the startup path (`WinMain`, window, configuration, file I/O) runs to the main loop on a recording stub D3D9 device, with `lua5164` translated (see "R4 design"). |
| **R5 Graphics** | D3D9 subset + D3DX over Vulkan (`R5a` System Vulkan + P5 `GLShaders` → SPIR-V + base forward rendering; `R5b` cached ETC2/ASTC texture transcoding + `fx_2_0` SM2/3 → SPIR-V for custom mod effects; `R5c` PanVK/Turnip `AHardwareBuffer` + `ASurfaceControl` bridge); HDR/DOF/MSAA advertised unavailable until post-v1. |
| **R6 Audio, input, touch** | 54 `fmodex64` exports shimmed onto P6 `WseAudioMixer` + SDL3 audio (`lua5164` already translated in R4); DInput/XInput from SDL; Vulkan touch overlay inside `Present()` with per-scene context switching (`ShowCursor` + PDB `g_gameScreen`), soft keyboard `WM_CHAR`, and gamepad auto-hide. |
| **R7 WSL x86-64 bring-up & save oracle** | Whole exe translated for x86-64 running in **WSL Ubuntu** with our shims only (Vulkan via WSLg or Lavapipe frame-dump + scripted input): main menu, campaign map, scene, battle. Savegame oracle: retail Windows saves load, and our saves load in retail Windows Warband. |
| **R8 Phone bring-up (v1 playable `Native`)** | On-phone translation proven against OOM stalls (the batch memory fix itself lands in the next native update, before R5; cold `-O1` test) plus PC-zip import; permanent `filesDir/wse2/user/` writable layer + Launcher Save Manager (`docs/PLATFORM_PLAN.md` "P14"); `Native` playable end-to-end on the reference phone (OPPO Reno 11, 12 GB, Mali-G68/PanVK + System Vulkan) hitting 30 FPS sustained for 30+ min in 100-troop battles (~720p / 85% scale) and 60 FPS in menus/map. |
| **R9 Performance, hardening & PC-parity mods** | Compiler & GPU acceleration for 1,000+ troop battles: register-passing ARM64 ABI + cross-batch inlining/ThinLTO, on-device PGO (hot ~5% at `-O3` + big-core tuning, cold ~95% at `-O1`), selective TSO relaxation & 128-bit SSE→NEON vectorization, indirect/instanced draw batching in D3D9→Vulkan; PDB-symbolized crash reports; `Native`, `Phantasy Calradia`, and `FiveDynastiesAndTenKingdoms` played end-to-end at PC mod parity. |
| **R10 WSE2 updates & player distribution** | Automatic pruning of stale `.so` caches on exe/translator change; plain missing-PDB / unshimmed-import diagnostics; GitHub Releases APK signed with a private release keystore (never `debug.keystore`; no Google Play) + standalone prebuilt Windows/Linux PC prep CLI zip (no WSL/compiler required). |
| **R11 Full PC visuals, modern mobile graphics & high refresh rate** | Full PC D3D9 parity (real-time shadow maps `R16F`/`R32F`, HDR tone-mapping + auto-exposure `A16B16G16R16F`, Depth of Field, water reflections); Vulkan spatial upscaling/sharpening (FSR 1.0 / CAS) and post-process AA (FXAA/SMAA); 90 Hz / 120 Hz sustained gameplay on flagship GPUs; Android HDR10 / Wide-Color swapchain output. |
| **R12 Native multiplayer (LAN, direct-IP & WSE2 servers)** | Real `WS2_32` POSIX/BSD non-blocking UDP/TCP sockets and Android TLS `WINHTTP`/`WININET` for Wi-Fi LAN, direct-IP hosts, and non-Steam WSE2 community servers (`steam_api64` remains stubbed). |

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

**Traps and the CPU profile.** `int3` (MSVC emits it after calls that never return), `int 0x29`
(`__fastfail`), `ud2` and `hlt` are lifted as a call to the `unsupported` hook: reaching one is a
fault with its address, and the function still counts as lifted. `cpuid` calls the `cpuid` hook,
which answers from one fixed profile (`Runtime::hook_cpuid`: vendor `WSE2AArch64 `, SSE through
SSE4.1 and POPCNT; no SSE4.2, AVX, FMA or OSXSAVE). Code that needs a feature the profile does not
advertise (VEX/EVEX encodings, SSE4.2, `xgetbv`) sits behind a feature check that answers no, so
it is lifted as a fault too and counted separately by the coverage meter.

R1 deviations (each also noted next to its code): `rsqrt*`/`rcp*` return the exact `1/sqrt(x)` and
`1/x` (x86 returns an approximation within 1.5 * 2^-12 whose bits differ between vendors); a `lock` operand is assumed naturally aligned
(AArch64 atomics need it; x86 would bus-lock a misaligned one); `cvtsd2ss`/`cvtpd2ps` round to
nearest whatever MXCSR.RC says; string instructions with a `gs:`/`fs:` override and 16-bit
addressing are unsupported.

**NaN results are x86's** (found by the arm64 corpus job, 2026-11-19): AArch64 makes a positive
default NaN where x86 makes the negative "real indefinite", LLVM may commute `fadd`/`fmul` and
leaves NaN payloads unspecified, and its AArch64 backend turns `select(fcmp)` min/max against a
constant into `fmaxnm`/`fminnm`, which turn a signaling-NaN input into a NaN. So every SSE
arithmetic result is rebuilt from the integer bits by Intel's rule (first source's NaN quieted,
else the second's, else real indefinite; SDM vol. 1 table 4-7), and min/max quiet their first
operand first. Translated AArch64 code now matches the x86 results bit for bit on the corpus.

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

## R2 design (2026-11-19)

Decided with the user: the PC records native results and the phone checks its translations
(this PC has no LLVM and no WSL); self-contained retail functions first; the recorded table is
committed; CI adds an arm64 runner. Measured on v1.1.5.6: of 14,644 `.pdata` ranges, 3,829 make
no call and 2,963 of those touch no `rip`-relative or `gs:` memory.

**Selection** (`engine/src/recomp/r2_select.{h,cpp}`, shared by the PC tool and the phone, so both
pick the same set): entries as in the coverage meter (`collect_function_entries`); a function is
*self-contained* when its body decoded completely and it has no call, tail call, indirect jump
other than a resolved jump table, `fs:`/`gs:` operand, write through a `rip`-relative or absolute
address, trap, `cpuid`, x87, privileged or cpuid-gated instruction. Reads of the image (`.rdata`
constants, `.data` as loaded) are allowed: both sides map the same file.

**Inputs** (`wse/recomp/r2_rig.h`, header-only): 16 cases per function, each seeded from the VA
and the case number. Four 64 KiB buffers with guard pages on both sides; every argument register
(`rcx rdx r8 r9`) and eight stack arguments are, by case, a pointer near the middle of a buffer, a
small integer or an edge value; `xmm0`-`xmm3` hold finite floats. Buffers are random bytes in which
every 64th aligned word is a pointer into a buffer, so pointer-chasing code reads real memory.

**Outputs.** `rax`, the low 64 bits of `xmm0`, the four buffers, and whether the call faulted.
Pointers are side-specific, so any 64-bit value (in `rax` or in an aligned buffer word) that points
into a buffer or the stack is replaced by `(region, offset)` before hashing. A fault is one result
(no detail): a wild pointer faults on both sides. One FNV-1a hash per function over all cases.

**PC side** (`tools/r2_pc_record.cpp`, Windows, local only): maps the exe at its base, calls each
selected function through an assembly thunk that loads every argument register, `xmm0`-`xmm3` and
the stack arguments; a vectored exception handler turns a fault into a result; a watchdog thread
records a function that runs over 2 s as skipped and restarts the tool, which resumes. It writes
`engine/src/recomp/r2_reference_v1156.inc` (`{va, hash}` per function, skipped ones omitted).

**Phone side** (Advanced > "R0 spikes", section "R2 differential"): for the v1.1.5.6 exe, selects
the same functions, lifts them in batches with `R1Translator`, links and loads each batch, runs the
same cases with `CpuState` (a `sigsetjmp` guard turns a fault into a result, `SIGALRM` a timeout),
and reports matches, mismatches (with PDB names), functions not lifted, and timeouts.

**CI.** `recomp_llvm_test --record <file>` (x86 `recomp-host`) writes per-export corpus result
hashes, normalized the same way, and uploads them with the corpus PE; job `recomp-arm64`
(`ubuntu-24.04-arm`, apt LLVM 18) translates that PE for AArch64 and checks it against the file.

## R3 design (2026-11-19)

Decided with the user: exceptions take the fastest design; memory ordering is correct first;
CI tests use the corpus plus hand-written SEH routines. Measured on v1.1.5.6: 2,006 functions use
`__CxxFrameHandler3`, 514 the stack-cookie handlers (`__GSHandlerCheck`, `_EH`, `_SEH`), 72 one
more handler; the exe imports `RaiseException`, `RtlUnwindEx`, `RtlVirtualUnwind`,
`RtlLookupFunctionEntry`, `RtlCaptureContext`, `CreateThread`, `TlsAlloc`/`TlsGetValue`,
`FlsAlloc`, and has a TLS directory; 201 functions use `lock`.

**Exceptions: emulate the Windows layer.** The exe carries its own C++ exception logic
(`__CxxFrameHandler3` and the rest of the statically linked vcruntime), which R1 translates like
any other code; the runtime implements only what Windows provides under it: `RaiseException`
(phase 1: walk guest frames with the exe's `.pdata`/`.xdata`, calling each frame's language
handler), `RtlUnwindEx` (phase 2: call the handlers again with `EXCEPTION_UNWINDING`, then restore
the target context), `RtlVirtualUnwind`, `RtlLookupFunctionEntry`, `RtlCaptureContext`. Guest
frames keep their Win64 layout on the guest stack, so the unwinder works on guest memory exactly
as Windows does. Chosen for speed: the code that does not throw pays nothing new. After every
call, translated code already compares the returned `rip` with the call's return address; only
when they differ does it look at `CpuState::unwind`: if the runtime is unwinding to this frame
(the call's return address and guest `rsp` match the target), the function jumps to its *resume
switch* (one case per block, on `rip`) and continues at the catch continuation; otherwise it
returns at once, so the host stack unwinds through ordinary returns (no host C++ exceptions, no
`setjmp`). Funclets are `.pdata` functions and are dispatched like any other.

**Exceptions inside exception handling (built).** The runtime records each call it makes into
guest code (a phase-1 handler, a termination handler, a consolidation callback) as a per-thread
boundary, and a frame walk that unwinds out of one crosses into the frames Windows would find
there: past a phase-1 handler it continues at that raise's thrower with `EXCEPTION_NESTED_CALL`
up to and including the frame whose handler raised (a nested exception); past a termination
handler it examines the frame being unwound again with its saved `ScopeIndex`, adding
`EXCEPTION_COLLIDED_UNWIND` in phase 2 (a collided unwind); past a consolidation callback (a C++
catch block) it continues at the catch's frame, so a rethrow from a catch (145 v1.1.5.6 catch
funclets end in one) reaches the frames above it. The corpus cases (`t_cxx_rethrow`,
`t_seh_nested_raise`, `t_seh_collided`) were also run natively on Windows (the corpus PE
loaded as a module, its IAT bound by the test): Windows returns the same results.

**Catch continuations (discovery).** A C++ catch funclet returns the address to continue at
(`lea rax, [rip + X]`), and nothing else reaches it, so function discovery adds these addresses
as blocks (`find_eh_continuations` in `engine/src/recomp/discover.h`). Every `.pdata` function
whose handler data starts with the RVA of a `FuncInfo` (magic `0x19930520`-`22`) contributes its
try-block map; each catch funclet is discovered and its `lea rax, [rip + X]` targets that lie
inside a `.pdata` function (not at an entry) become blocks of that function (chained fragments
resolve to their primary). The handler routine is not identified by name, so the rule needs no
PDB. Measured on v1.1.5.6: 1,921 `FuncInfo`, 180 catch funclets; 35 return a continuation
(33 distinct, in 28 functions) and the other 145 all end in a rethrow or a no-return call; no
body that decoded before fails with the new blocks.

**Memory ordering: correct first.** A guest access whose address is based on `rsp` is a stack
access and stays plain. Every other 2/4/8-byte load is an acquire load and every store a release
store (`LDAPR`/`LDAR`, `STLR` on AArch64; plain `mov` on x86, which already is TSO) when the
address is naturally aligned at run time, else a plain access with a barrier (`dmb ishld` after a
load, `dmb ish` before a store); byte accesses are always aligned. 16-byte SSE accesses are plain
with the same barriers. `lock` instructions stay sequentially consistent atomics. The cost is
measured on the phone, then provably thread-local data is relaxed.

**Imports: the ABI bridge.** The runtime names every IAT slot from the PE import directory
(`KERNEL32.dll!TlsAlloc`), so shims register by name; a shim reads its Win64 arguments from
`CpuState` (`rcx rdx r8 r9 xmm0-3`, the rest at `[rsp + 0x28 ...]`), writes `rax`/`xmm0`, and
returns through `Runtime::return_from_import`. Host code calls back into guest code (window
procedures, thread entry points) with `call_guest(state, va, args...)`, which builds a Win64 frame
on the guest stack. `Runtime::fill_iat` writes each slot's own VA into it, as the loader writes the
imported address: compilers load an import into a register and call it later (clang hoists
`__imp_*` loads out of loops; found by the threads corpus), and such a call reaches `dispatch` with
the slot VA, which runs the import.

**Threads and TLS.** Each guest thread has its own `CpuState`, a guest stack, and a TEB in guest
memory (`gs:` reads go through `segment_base`): self pointer at `+0x30`, stack base/limit at
`+0x8`/`+0x10`, the TLS slot array at `+0x1480` and the static-TLS pointer array at `+0x58` (from
the exe's TLS directory). `CreateThread` starts a host thread running the guest entry point.

Built (`engine/include/wse/recomp/teb.h`): the `gs:` base is per thread, `CpuState::teb` (offset
440, in what was padding, so the 448-byte layout and the lifter are unchanged); `segment_base`
returns it. Measured on v1.1.5.6: only three instructions use a segment, `gs:[0x58]` (static TLS,
2 functions) and `gs:[0x10]` (stack limit, 1), so the hook call costs nothing that matters and no
inline `gs:` load was added. The TLS directory holds an 8-byte template and no callbacks. The
PEB is a zeroed page holding the image base at `+0x10`. `TlsAlloc` slots 0-63 are in the TEB,
64-1087 in a per-thread expansion array (`+0x1780`) allocated on first use; FLS values are kept
per thread on the host side (guest code reaches them only through `Fls*`), and their callbacks run
on `FlsFree` (for every thread's value, on the calling thread) and on thread exit, followed by the
TLS callbacks with `DLL_THREAD_DETACH`. `GetLastError`/`SetLastError` use `LastErrorValue`
(`+0x68`). Thread ids start at `0x1004` in steps of 4; the process id is `0x1000`.

**Threads, handles and sync (built, `threads.h` and `kernel.h`).** Decided with the user: threads
come with the handle table and the synchronization calls the exe uses, so translated threads
can coordinate; `TerminateThread` of another thread is deferred. Measured call sites in
v1.1.5.6: `CreateThread` 4, `ExitThread` 4, `GetExitCodeThread` 13, `TerminateThread` 4,
`WaitForSingleObject` 19, `CloseHandle` 33, `EnterCriticalSection` 185, `LeaveCriticalSection`
195, `Sleep` 54, `SetEvent` 30, SList calls 80, condition-variable calls 16. Each guest thread
runs on a host thread with its own `CpuState`, a guest stack (the requested size, at least
1 MiB) and a TEB; it runs the TLS callbacks with `DLL_THREAD_ATTACH`, its entry point through
`call_guest`, then its FLS callbacks and `DLL_THREAD_DETACH`, and its handle signals with the
exit code. `ExitThread` sets `unwind_rsp` to 1, which matches no frame's call-site `rsp`, so
every translated frame returns at once. A thread marked by `TerminateThread` exits the next
time it calls an import (`Runtime::before_import`), waits or sleeps. Kernel objects (thread,
event, mutex, semaphore, named objects shared) live in one host handle table; one host mutex
and condition variable serve every wait. A critical section keeps a host lock whose pointer is
in its `LockSemaphore` field and mirrors `OwningThread`/`RecursionCount`; a condition
variable's word holds a host object created on first use; SLists keep the x64 16-byte header
(depth and sequence in word 0, first entry in word 1) under one host lock.

**Thread-pool timers and waits (built, `threadpool.h`).** `CreateThreadpoolTimer`/`Wait`,
`SetThreadpoolTimer`/`Wait`, `WaitForThreadpool*Callbacks`, `CloseThreadpool*`,
`RegisterWaitForSingleObject`, `UnregisterWait(Ex)` (1-3 call sites each in v1.1.5.6). Each object
has one worker, a guest thread with its own TEB, stack and TLS attach/detach callbacks (as Windows
pool threads are); it sleeps until the object is due and calls the callback. Waits use the
kernel's wait, so an auto-reset event is consumed. The corpus cases were also run natively on
Windows with the same results.

**Tests (CI, synthetic).** The corpus gains assembly functions with `.seh_handler` routines
written in C: they raise, catch, unwind through several frames and resume, on the x86 and arm64
runners. The exe's real `__CxxFrameHandler3` is checked by the phone/PC rig.

**Order:** memory ordering → import bridge (names, argument helpers, `call_guest`) → unwinder and
exception dispatch with the lifter's resume switch → TEB/TLS → threads.

## R4 design (2026-11-19)

Decided with the user (interview, 2026-11-19):

| Question | Decision |
| --- | --- |
| Where bring-up runs | **WSL Ubuntu on the user's PC.** The user installs WSL + Ubuntu (admin rights; the agent changes no system settings); the recompiler builds there exactly as in CI (`WSE_ENABLE_LLVM=ON`, apt LLVM 18) and the retail exe runs locally on Linux x86-64 under gdb. Retail code stays local; CI still sees only synthetic code. |
| Where R4 stops | **A recording stub D3D9 device**: `Direct3DCreate9` and D3DX return COM objects whose methods succeed and are logged, so startup reaches the main loop and frames tick headless. R5 replaces the stub with the Vulkan-backed device. |
| Method | **Hybrid**: the high-count families first, each with host tests and corpus cases; then run startup and implement each next missing import in the order it is hit. |
| Files | **One `Vfs`, two backends**: a host-directory backend (the install, case-insensitive, `\` separators) for bring-up and the SAF backend on the phone; writes (saves, `rgl_config.txt`, logs) go to a per-user writable layer. |
| Network and Steam | **Offline stubs**: sockets and HTTP fail with their real error codes; `steam_api64` behaves as absent. |
| Window and messages | **Fake `HWND`, headless**: `CreateWindowExW` returns a fake window whose `WndProc` the shims call; the message queue is ours (fed from SDL events later). |
| FMOD | **A silent FMOD-API stub** (valid opaque handles, every call succeeds, nothing plays) until R6 translates `fmodex64`. |
| Lua | **Translate the shipped `lua5164.dll` in R4** (pulled forward from R6): startup initializes Lua. |

**Measured import surface (v1.1.5.6, local census; call sites are instructions that reference
the import slot).** 482 static imports in 21 DLLs plus 64 delay-loaded (`fmodex64` 54,
`steam_api64` 10). R3 already shims 72 `KERNEL32` functions.

| DLL | Functions | Call sites | R4 owner |
| --- | --- | --- | --- |
| `lua5164` | 103 | 3,800 | translated (697,344 B, base `0x180000000`, 1,552 relocations, 2,257 `.pdata`, 148 exports, no PDB, no TLS; its only imports are 105 `KERNEL32`) |
| `KERNEL32` | 223 (72 shimmed) | 1,811 | families below |
| `USER32` | 40 | 71 | window and messages (fake `HWND`), clipboard, cursor, `MessageBoxA` (logged) |
| `WINMM` | 3 | 70 | `timeGetTime`, `timeBeginPeriod`/`EndPeriod` (time family) |
| `WS2_32` | 37 (28 by ordinal) | 45 | offline stub |
| `d3dx9_43` | 17 | 41 | stub device helpers; math functions (`D3DXMatrix*`, `D3DXVec*`) real |
| `ADVAPI32` | 10 | 15 | registry (an in-memory hive persisted to the writable layer), `Crypt*` hashing (real MD5/SHA-1) |
| `WINHTTP`, `WININET`, `urlmon` | 13 | 16 | offline stub |
| `SHELL32`, `SHLWAPI` | 6 | 17 | `SHGetFolderPathA` → writable-layer folders; `ShellExecuteA` logged, fails |
| `IMM32`, `dbghelp`, `ole32`, `OLEAUT32`, `PSAPI`, `GDI32`, `d3d9`, `DINPUT8`, `XINPUT1_3` | 25 | 26 | IME off; `StackWalk64` and crash dumps report "unavailable"; COM init; `Direct3DCreate9` → stub; `DirectInput8Create` → stub device; XInput "no controller" until R6 |

**`KERNEL32` families still to shim (151 functions).** Each is one header under
`engine/include/wse/recomp/`, bound by name like R3's:

| Family | Main functions (call sites) | Notes |
| --- | --- | --- |
| Time and counters | `QueryPerformanceCounter` 110, `GetSystemTimeAsFileTime` 21, `GetTickCount` 15, `QueryPerformanceFrequency` 7, local/system time conversions, time zone | monotonic host clock; QPC frequency 10 MHz |
| CRT support | `DecodePointer` 42, `EncodePointer` 35, `IsDebuggerPresent`, `IsProcessorFeaturePresent`, `SetUnhandledExceptionFilter`, `RtlPcToFileHeader`, `InitOnceExecuteOnce`, command line, startup info, environment, `ExitProcess`/`TerminateProcess` | pointers encoded with a per-process cookie as Windows does |
| Strings and locale | `MultiByteToWideChar` 34, `WideCharToMultiByte` 21, `FormatMessageA/W` 8, code pages (1252 as ACP, 437 as OEM), `LCMapStringEx`, `CompareStringEx`, `GetLocaleInfoEx`, date and time formats | `en-US` locale; UTF-8 code page supported |
| Files and directories | `FindFirstFileA` 46, `FindClose` 41, `CreateDirectoryA` 22, `ReadFile`/`WriteFile` 16, `DeleteFileA` 7, `GetFullPathNameA` 6, attributes, current directory, temp path, file mapping, locks, std handles and console | through `Vfs` (two backends + writable layer) |
| Modules and resources | `GetProcAddress` 52, `GetModuleHandleW/A/ExW` 37, `LoadLibrary*` 14, `GetModuleFileNameA/W` 7, `FreeLibrary`, `FindResource*`/`LoadResource`/`LockResource`/`SizeofResource` | a module registry: translated modules (exe, `lua5164`) and synthetic ones (system DLLs, the FMOD and Steam stubs); delay loading needs nothing more (its helper is statically linked) |
| Memory | `Heap*` (~30), `Global*`/`Local*` 14, `VirtualAlloc`/`Free`/`Protect`/`Query` (lua), `GlobalMemoryStatusEx` | heaps over the host allocator; virtual memory from a reserved guest range |
| System information | `GetSystemInfo`, `GetNativeSystemInfo`, `GetLogicalProcessorInformation`, `GetProcessAffinityMask`, `GetVersionExW` (Windows 10), `IsWow64Process` | the fixed CPU profile of `Runtime::hook_cpuid` |
| Timer queues | `CreateTimerQueue(Timer)` 4, `ChangeTimerQueueTimer` 2, `DeleteTimerQueueTimer` 4 | on `threadpool.h` |
| Process | `CreateProcessA` 1 | fails (logged) |

**Runtime work R4 needs beyond shims.** Several translated modules: each image maps at its
preferred base when free (relocations otherwise), is translated by its own `R1Translator` batch,
and adds its table to the one `Runtime`; `seh::Exceptions` looks up `.pdata` across all images;
`lua5164` adds `RtlRestoreContext` and `RtlUnwind` (C `longjmp` and SEH unwinds) to the SEH layer.
A translated DLL's `DllMain` (`_DllMainCRTStartup`) runs with `DLL_PROCESS_ATTACH`, and its TLS
directory is read like the exe's.

**Slice 2 as built (2026-11-19, `wse/recomp/shim_modules.h`).** One `shim::Modules` registry per
process. A translated module's handle is its base; a system DLL (KERNEL32, KERNELBASE, ntdll,
every DLL an image imports, and any DLL `LoadLibrary` names that has shims) is a 64 KiB fake image
with headers only, and `GetProcAddress` on it hands out an address inside it per shimmed function,
which `Runtime::dispatch_fallback` runs (under the registry's lock only for the lookup, so any guest
thread may call it). Shims register by name (`Runtime::set_import` keeps `dll!name`), so an image
added later binds when it is named. `link` binds each import from another translated module to the
export (`Runtime::bind_import_to`: the IAT slot holds the export's VA, and a call through it
dispatches there), following forwarders. `attach_dlls` calls each statically imported DLL's entry
point with `DLL_PROCESS_ATTACH` and a non-null `lpReserved`, dependencies first. `seh::Exceptions`
takes every translated image (`add_image`) and picks the one containing each frame's `rip`. The
lua5164 SEH entry points: `RtlUnwind` (as `RtlUnwindEx` without a context) and `RtlRestoreContext`
(resume through `unwind_rsp`; `STATUS_LONGJUMP` takes the nonvolatile registers from the jump
buffer). A resumable function's call return addresses are block starts, so such a restore can
continue at a `setjmp` call site. Still open in this slice: translating `lua5164` from the user's
install (each `setjmp` caller must have a `.pdata` handler to be resumable), and reading the DLLs'
TLS directories.

**`lua5164` is LuaJIT 2.1** (measured on the shipped DLL, 2026-11-20: version string
`LuaJIT 2.1.1734355927`, GC64 build, `luaJIT_setmode` exported). Two consequences:

- *The JIT is off.* LuaJIT compiles hot Lua to x86 machine code at run time; nothing can run that
  here, and translating during play is banned. Every state the exe creates gets
  `luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF)` right after `luaL_newstate`, so Lua
  runs in LuaJIT's interpreter.
- *The interpreter's handlers come from its offset table.* LuaJIT's VM is one `.pdata` function
  (16,161 bytes at its start) whose bytecode handlers jump to each other through a table
  `lj_dispatch_init` fills at run time from 16-bit offsets (`lj_bc_ofs`, 156 entries). Static
  discovery finds them from the code that adds those offsets to the VM's start
  (`find_offset_table_entries` in `discover.h`: a general rule, no LuaJIT names). Handler-to-handler
  jumps are tail calls in the translation (`musttail` to a function of the module, `tail` through the
  dispatch hook), so the interpreter runs in constant host stack.

**Bring-up tool (local only).** `tools/r4_run.cpp`: translates the exe and `lua5164` from the
user's install, maps them, binds every shim, runs the CRT entry point on the main guest thread, and
logs the first call of each import; a missing import faults with its DLL and name, which is the
next thing to implement. Never run in CI.

**Slices, in order:**

1. **WSL bring-up environment**: WSL Ubuntu + apt LLVM 18 / lld 18 / clang 18; the CI build and
   all gates pass there; the recompiler gate runs in WSL.
2. **Multi-module runtime**: module registry, mapping with relocations, `lua5164` translated,
   cross-module calls and unwinding, `DllMain`, `GetModuleHandle`/`GetProcAddress`/`LoadLibrary`.
3. **High-count families with tests**: time, CRT support, strings and locale, memory, files and
   directories (`Vfs` host backend + writable layer), system information, timer queues.
4. **Startup run**: `tools/r4_run.cpp` on the retail exe; implement each next missing import
   (USER32 fake window and messages, ADVAPI32 registry, SHELL32 folders, the offline network and
   Steam stubs, the silent FMOD stub, the recording D3D9/D3DX/DInput stubs).
5. **Done when**: the retail v1.1.5.6 exe, translated in WSL, starts with Native and runs its
   main loop for 1,000 frames on the stub device with no fault and no missing import, and the
   same build runs from the APK's Advanced screen on the phone.

**Phone run (decided with the user, 2026-11-21).** Advanced > "Game test" (named "R4 startup
run" until 2026-11-21, when its texts were put in plain words for players; "R0 spikes" became
"Phone check" and the raw report lines moved under "Details for the developers") runs the same
startup on the phone:

- **One runner.** `engine/src/recomp/r4_startup.{h,cpp}` (`run_startup`, in `wse_recomp`) does what
  `r4_run` did: discovery, batched translation with the batch cache, mapping, linking, shims,
  diagnostics, the run. `tools/r4_run.cpp` (WSL) and `platform/android/r4_jni.cpp` (in
  `libwse_r0.so`) only build its inputs: the exe, `lua5164.dll` and PDB bytes, an install `Vfs`, a
  writable `Vfs`, and a translator id.
- **Its own process.** `R4StartupService` runs in `android:process=":recomp"` (as the Vulkan probe
  does): the images map at fixed addresses (possible once per process), guest threads cannot be
  stopped, and a crash in translated code must not take the launcher down. The run ends the process
  (`_Exit`); the launcher sees the binding die and reads `filesDir/recomp/r4/report.txt`, whose last
  `RESULT` line says how it ended (`pass N frames`, `fault`, `host-signal`, `exit-process`,
  `returned`).
- **Scope.** Native (seeded as `last_module_warband`, as the launcher does), stop after 1,000
  frames or the first fault, then a report with Copy / Share.
- **Install files, two ways.** The SAF folder through `WseSafBridge` (a backend that attaches each
  guest thread to the JVM, behind a lock: `SafVfs` is not thread-safe), or, with the switch "R4:
  use a copy of the install", a copy in `filesDir/recomp/r4/install` (everything except the other
  modules in `Modules/`; a file already copied with the same size is kept) read as plain files. The
  writable layer is `filesDir/recomp/r4/user`.
- **Cache.** Batches in `filesDir/recomp/r4/so`, keyed like `r4_run`'s by the image, the options and
  the translator id (the size and an FNV-1a hash of `libwse_r0.so`, so a new APK retranslates);
  an interrupted translation resumes. `progress.txt` holds the current image and its batches done.
- **Translation.** Every core (changed from all but one by the user, 2026-11-21; the thread count is
  not in the cache key), `-O2` (as verified in WSL). The worker's threads inherit the highest
  priority Android lets the app set, and the worker is bound with `BIND_IMPORTANT |
  BIND_ABOVE_CLIENT`, so it gets the launcher's priority. While the run goes on, the launcher keeps
  the screen on (any tab), so it stays the top app (the big cores); the first run asks to exempt the
  app from battery optimization (Android's own dialog).
- **In the background (decided with the user, 2026-11-21).** The run goes on when the user leaves
  the app, turns the screen off, or swipes the app from Recents. `R4ForegroundService` (a
  `specialUse` foreground service in the launcher's own process, which holds the binding to
  `:recomp`) keeps that process, and through the binding the worker, alive and holds a partial wake
  lock (at most 3 h). Its notification shows the status line and has Stop, which ends the worker
  (cached batches are kept, so the next run resumes). `R4StartupRunner` is one per launcher
  process, so the run outlives the activity. Every ending reaches the launcher as the binding dying,
  so it always posts a result notification ("PASS: …" / "Stopped: …"), which opens Advanced with
  the report, read again from `report.txt` if the launcher process was restarted. Android 13+ asks
  for the notification permission when the run starts; denied, the run still goes on, silently.
- **Resuming by itself (2026-11-21, after the user saw the run vanish at 38/80).** When the worker
  ends without a `RESULT` line (Android ending it, in practice for memory on every core),
  `R4StartupRunner` binds it again after 3 s with half the threads (at least 2, or 1 after a death
  that cached no new batch; kept in preferences for later runs) and gives up only after 3 deaths in
  a row with no new batch. A killed launcher process resumes the run through the sticky
  `R4ForegroundService` restart or the next app start, and that counts as a worker death too (the
  batch count at the last bind, the deaths in a row and the resumes are kept in preferences): on
  the phone the run sat at 77/80 for 1.5 h, its last attempt starting "3 to translate on 4
  threads" with no resume shown: most likely the whole app was killed again and again, each time
  restarting the three largest batches (each 3-3.5x the average size) from zero. The status counts the whole image (cached
  plus this attempt's batches), read from the report's per-image summary line. The first attempt
  uses one thread per core but at most one per 1.2 GB of free memory (`ActivityManager` `availMem`):
  `tools/r4_run` took ~1.8 GB on 2 threads for this exe, and the phone run on every core stalled at
  50/80, as swapping would explain.
- **Loading.** Batches load from a `memfd` (`android_dlopen_ext`), else with our own ELF loader
  (`code_loader.h`, shared with the R0 spikes): on the phone `dlopen` of a read-only app-data file
  failed in R0 run 2. Guest images are mapped read-write only (translated code runs, guest bytes
  never execute).

## Preparing on a PC (2026-11-21)

Decided with the user after the phone's first full translation took most of a day (the PC is ~2.4x
faster per batch, and does not get killed for memory). Still the user's own exe, translated on
their own machine for their own phone: nothing is bundled or shared, and the zip says so.

- **One target for both.** The phone translated for its own CPU (`getHostCPUName` and its
  features), which a PC cannot know. Both now use `portable_android_target()`: triple
  `aarch64-unknown-linux-android`, CPU `generic`, `+v8.2a,+rcpc` (LSE atomics for `lock`
  instructions, LDAPR for acquire loads: the R3 choices). A phone without LSE or RCpc keeps its
  own CPU, so its batches never match a PC's and are translated there.
- **The key no longer hashes the library.** A batch's key held a hash of `libwse_r0.so` (phone) or
  the size and time of `libwse_recomp.a` (PC), so the two never agreed and every shim change
  retranslated. It now holds `translator_fingerprint()`: a SHA-256 of the lifter's sources
  (`_wse_lifter_sources` in `CMakeLists.txt`, line endings removed so a Windows checkout and CI's
  agree), the LLVM version and the target. Shims are not in it, so a shim-only update keeps the
  phone's batches; a source that changes translation must be in that list.
- **PC side:** `tools/r4_run --for-phone <dir>` translates both images for that target and runs
  nothing; players use `tools/prepare_for_phone.bat` (it checks WSL, finds or asks for the
  Warband folder, installs the build tools and builds `r4_run` once, translates with as many
  threads as WSL's memory allows, and writes `Downloads\wse2-prepared-for-phone.zip`).
- **Phone side:** the player copies the zip into the Warband folder they picked in the app. When
  the game test starts, `R4StartupRunner.importPrepared` finds `wse2-prepared*.zip` in that
  folder's root and copies its `.so` / `.key` files into the batch cache (once per zip: name, size
  and time are remembered). Each imported batch is checked against its key like any cached one,
  so a zip from another exe or translator version is translated again on the phone, not used.
  A batch's `.so` and `.key` are installed together, the old `.key` removed first and the new one
  written last, so an import stopped part-way never leaves a key vouching for other code; a batch
  whose key is already there unchanged is skipped (review fix, 2026-11-21).
- **Saving the report:** Advanced > "Save the game test report" writes the report (during a run:
  what it has written so far, under its status) to a file the player picks.
  No USB debugging, adb or button is needed.

## R5 census (2026-11-21)

Measured locally in WSL against the retail v1.1.5.6 `mb_warband_wse2_x64.exe` + `lua5164.dll`
with `Native` selected, reusing the existing translated batch cache (`/root/wse/r4/so`, 0 batches
retranslated), stopping at `1,000` presented frames (`321.8 s`, `0` faults, `458,394` import calls
across `163` distinct imports). Findings only — no R5 code is added here.

**Headline findings:**

- **Missing COM methods:** **0**. Every D3D9, D3DX, and DirectInput COM method invoked across
  startup and 1,000 frames is already implemented or recorded by `shim_d3d9.h`.
- **Non-`S_OK` HRESULTs:** **1** (`IDirect3D9::CheckDeviceMultiSampleType`, `666` calls returning
  `D3DERR_NOTAVAILABLE` / `0x8876086a`, expected because the stub advertises only
  `D3DMULTISAMPLE_NONE`).
- **100% shader-driven path:** `0` calls to `SetTransform`, `0` calls to `SetTextureStageState`,
  `0` calls to `CreateQuery`, and `0` calls to `DrawPrimitive*UP`. Every draw goes through
  `ID3DXEffect` + `IDirect3DStateBlock9` + `DrawIndexedPrimitive(D3DPT_TRIANGLELIST)` (`5` draws
  per frame on the main-menu loop).

### COM methods & graphics DLL imports (76 COM methods, 7 DLL imports)

Top 10 most-called graphics/effect methods (COM + `d3dx9_43.dll`):

| Rank | Method | Calls | Status |
| ---: | --- | ---: | --- |
| 1 | `IDirect3DStateBlock9::Apply` | 20,036 | recorded |
| 2 | `ID3DXEffect::SetBool` | 17,000 | recorded |
| 3 | `ID3DXEffect::SetMatrixTranspose` | 17,000 | recorded |
| 4 | `ID3DXEffect::SetFloat` | 16,003 | recorded |
| 5 | `IDirect3DDevice9::SetRenderState` | 13,074 | impl |
| 6 | `d3dx9_43.dll!D3DXMatrixMultiply` | 11,000 | DLL import |
| 7 | `ID3DXEffect::SetVector` | 10,001 | recorded |
| 8 | `ID3DXEffect::SetTexture` | 6,000 | recorded |
| 9 | `ID3DXEffect::CommitChanges` | 5,000 | recorded |
| 10 | `IDirect3DDevice9::DrawIndexedPrimitive` | 5,000 | recorded |
| 10 (tie) | `IDirect3DDevice9::SetStreamSource` | 5,000 | impl |

Full breakdown by interface / DLL:

| Interface / DLL | Method | Calls | Status |
| --- | --- | ---: | --- |
| `d3d9.dll` | `Direct3DCreate9` | 1 | DLL import |
| `d3dx9_43.dll` | `D3DXMatrixMultiply` | 11,000 | DLL import |
| `d3dx9_43.dll` | `D3DXMatrixOrthoOffCenterRH` | 1,000 | DLL import |
| `d3dx9_43.dll` | `D3DXCreateTextureFromFileExA` | 705 | DLL import |
| `d3dx9_43.dll` | `D3DXCreateEffectFromFileA` | 2 | DLL import |
| `d3dx9_43.dll` | `D3DXFilterTexture` | 2 | DLL import |
| `DINPUT8.dll` | `DirectInput8Create` | 1 | DLL import |
| `IDirect3D9` | `CheckDeviceMultiSampleType` | 918 | impl (`666` × `D3DERR_NOTAVAILABLE`) |
| `IDirect3D9` | `CheckDeviceFormat` | 225 | impl |
| `IDirect3D9` | `CheckDepthStencilMatch` | 216 | impl |
| `IDirect3D9` | `CheckDeviceType` | 36 | impl |
| `IDirect3D9` | `EnumAdapterModes` | 8 | impl |
| `IDirect3D9` | `GetAdapterModeCount` | 4 | impl |
| `IDirect3D9` | `GetDeviceCaps` | 3 | impl |
| `IDirect3D9` | `GetAdapterDisplayMode` | 2 | impl |
| `IDirect3D9` | `GetAdapterIdentifier` | 2 | impl |
| `IDirect3D9` | `GetAdapterCount` | 1 | impl |
| `IDirect3D9` | `CreateDevice` | 1 | impl |
| `IDirect3DDevice9` | `SetRenderState` | 13,074 | impl |
| `IDirect3DDevice9` | `DrawIndexedPrimitive` | 5,000 | recorded (`D3DPT_TRIANGLELIST`) |
| `IDirect3DDevice9` | `SetStreamSource` | 5,000 | impl |
| `IDirect3DDevice9` | `SetIndices` | 2,000 | impl |
| `IDirect3DDevice9` | `SetStreamSourceFreq` | 2,000 | recorded |
| `IDirect3DDevice9` | `SetViewport` | 2,000 | impl |
| `IDirect3DDevice9` | `Clear` | 1,001 | recorded (`D3DCLEAR_TARGET \| ZBUFFER`) |
| `IDirect3DDevice9` | `BeginScene` / `EndScene` / `Present` | 1,000 each | recorded / recorded / impl |
| `IDirect3DDevice9` | `SetVertexDeclaration` | 1,000 | impl |
| `IDirect3DDevice9` | `SetSamplerState` | 296 | impl |
| `IDirect3DDevice9` | `BeginStateBlock` / `EndStateBlock` | 64 each | recorded / impl |
| `IDirect3DDevice9` | `CreateVertexDeclaration` | 7 | impl |
| `IDirect3DDevice9` | `CreateTexture` | 5 | impl |
| `IDirect3DDevice9` | `ShowCursor` | 4 | recorded |
| `IDirect3DDevice9` | `GetDeviceCaps` | 3 | impl |
| `IDirect3DDevice9` | `CreateVertexBuffer` / `CreateIndexBuffer` | 2 each | impl |
| `IDirect3DDevice9` | `GetDepthStencilSurface` | 2 | impl |
| `IDirect3DDevice9` | `GetAvailableTextureMem` / `GetBackBuffer` / `GetDisplayMode` / `GetRenderTarget` / `SetCursorProperties` | 1 each | impl (4) / recorded (1) |
| `IDirect3DStateBlock9` | `Apply` / `Release` | 20,036 / 32 | recorded / impl |
| `IDirect3DVertexBuffer9` | `Lock` / `Unlock` | 1,004 each | impl / recorded |
| `IDirect3DIndexBuffer9` | `Lock` / `Unlock` | 4 each | impl / recorded |
| `IDirect3DTexture9` | `LockRect` / `UnlockRect` | 5 each | impl / recorded |
| `IDirect3DTexture9` | `GetLevelDesc` / `GetSurfaceLevel` | 1 each | impl |
| `IDirect3DSurface9` | `Release` / `GetDesc` | 3 / 2 | impl |
| `ID3DXEffect` | `SetBool` / `SetMatrixTranspose` | 17,000 each | recorded |
| `ID3DXEffect` | `SetFloat` | 16,003 | recorded |
| `ID3DXEffect` | `SetVector` | 10,001 | recorded |
| `ID3DXEffect` | `SetTexture` | 6,000 | recorded |
| `ID3DXEffect` | `CommitChanges` | 5,000 | recorded |
| `ID3DXEffect` | `IsParameterUsed` | 2,880 | impl |
| `ID3DXEffect` | `Begin` / `BeginPass` / `EndPass` / `End` / `SetInt` / `SetTechnique` | 2,000 each | impl (`Begin`) / recorded (5) |
| `ID3DXEffect` | `SetValue` | 1,000 | recorded |
| `ID3DXEffect` | `GetTechniqueByName` | 480 | impl (`480` distinct names) |
| `ID3DXEffect` | `GetParameterByName` | 164 | impl (`164` distinct names) |
| `ID3DXEffect` | `GetParameterDesc` | 2 | impl |
| `ID3DXEffect` | `Release` / `SetStateManager` / `SetVectorArray` | 1 each | impl / recorded / recorded |
| `IDirectInput8A` / `Device8A` | `GetDeviceState` / `GetDeviceData` / `CreateDevice` / `Acquire` / `SetCooperativeLevel` / `SetDataFormat` / `SetProperty` | 2,002 / 1,001 / 2 / 2 / 2 / 2 / 1 | impl (3) / recorded (4) |

### Render states (`19` distinct `D3DRS_*`)

| State | ID | Calls | Observed values (`value: count`) |
| --- | ---: | ---: | --- |
| `D3DRS_ALPHATESTENABLE` | 15 | 3,000 | `0 (FALSE): 2,000`, `1 (TRUE): 1,000` |
| `D3DRS_ZENABLE` | 7 | 1,008 | `0 (FALSE): 4`, `1 (TRUE): 1,004` |
| `D3DRS_CULLMODE` | 22 | 1,005 | `2 (D3DCULL_CW): 1,005` |
| `D3DRS_ALPHAREF` | 24 | 1,000 | `8: 1,000` |
| `D3DRS_ALPHAFUNC` | 25 | 1,000 | `5 (D3DCMP_GREATER): 1,000` |
| `D3DRS_FOGENABLE` | 28 | 1,000 | `0 (FALSE): 1,000` |
| `D3DRS_RANGEFOGENABLE` | 48 | 1,000 | `1 (TRUE): 1,000` |
| `D3DRS_CLIPPLANEENABLE` | 152 | 1,000 | `0 (none): 1,000` |
| `D3DRS_MULTISAMPLEANTIALIAS` | 161 | 1,000 | `0 (FALSE): 1,000` |
| `D3DRS_SLOPESCALEDEPTHBIAS` | 175 | 1,000 | `0 (0.0f): 1,000` |
| `D3DRS_DEPTHBIAS` | 195 | 1,000 | `0 (0.0f): 1,000` |
| `D3DRS_SRCBLEND` | 19 | 10 | `1 (ZERO): 2`, `2 (ONE): 2`, `5 (SRCALPHA): 6` |
| `D3DRS_DESTBLEND` | 20 | 10 | `2 (ONE): 6`, `3 (SRCCOLOR): 2`, `6 (INVSRCALPHA): 2` |
| `D3DRS_ALPHABLENDENABLE` | 27 | 10 | `0 (FALSE): 2`, `1 (TRUE): 8` |
| `D3DRS_BLENDOP` | 171 | 10 | `1 (D3DBLENDOP_ADD): 10` |
| `D3DRS_ZWRITEENABLE` | 14 | 8 | `0 (FALSE): 4`, `1 (TRUE): 4` |
| `D3DRS_ZFUNC` | 23 | 8 | `4 (D3DCMP_LESSEQUAL): 8` |
| `D3DRS_FILLMODE` | 8 | 4 | `2 (WIREFRAME): 2`, `3 (SOLID): 2` |
| `D3DRS_LIGHTING` | 137 | 1 | `0 (FALSE): 1` |

### Sampler states (`8` distinct `D3DSAMP_*`)

| Sampler state | ID | Calls | Stage mask | Observed values (`value: count`) |
| --- | ---: | ---: | --- | --- |
| `D3DSAMP_ADDRESSU` | 1 | 42 | `0xffff` (`0..15`) | `1 (WRAP): 14`, `3 (CLAMP): 26`, `4 (BORDER): 2` |
| `D3DSAMP_ADDRESSV` | 2 | 42 | `0xffff` (`0..15`) | `1 (WRAP): 14`, `3 (CLAMP): 26`, `4 (BORDER): 2` |
| `D3DSAMP_BORDERCOLOR` | 4 | 2 | `0x1000` (`12`) | `0: 2` |
| `D3DSAMP_MAGFILTER` | 5 | 42 | `0xffff` (`0..15`) | `1 (POINT): 5`, `2 (LINEAR): 29`, `3 (ANISOTROPIC): 8` |
| `D3DSAMP_MINFILTER` | 6 | 42 | `0xffff` (`0..15`) | `1 (POINT): 5`, `2 (LINEAR): 25`, `3 (ANISOTROPIC): 12` |
| `D3DSAMP_MIPFILTER` | 7 | 42 | `0xffff` (`0..15`) | `0 (NONE): 6`, `1 (POINT): 2`, `2 (LINEAR): 34` |
| `D3DSAMP_MIPMAPLODBIAS` | 8 | 42 | `0xffff` (`0..15`) | `0 (0.0f): 42` |
| `D3DSAMP_MAXANISOTROPY` | 10 | 42 | `0xffff` (`0..15`) | `1: 30`, `4: 12` |

### Resource creations & format probes

Resources created during startup and 1,000 frames:

| Creation call | Format | Pool | Usage | Calls |
| --- | --- | --- | --- | ---: |
| `D3DXCreateTextureFromFileExA` | `D3DFMT_DXT1` (`827611204`) | `D3DPOOL_MANAGED` (`1`) | `0` | 572 |
| `D3DXCreateTextureFromFileExA` | `D3DFMT_DXT5` (`894720068`) | `D3DPOOL_MANAGED` (`1`) | `0` | 123 |
| `D3DXCreateTextureFromFileExA` | `D3DFMT_DXT3` (`861165636`) | `D3DPOOL_MANAGED` (`1`) | `0` | 7 |
| `D3DXCreateTextureFromFileExA` | `D3DFMT_A8R8G8B8` (`21`) | `D3DPOOL_MANAGED` (`1`) | `0` | 2 |
| `D3DXCreateTextureFromFileExA` | `D3DFMT_R5G6B5` (`23`) | `D3DPOOL_MANAGED` (`1`) | `0` | 1 |
| `CreateTexture` | `D3DFMT_A8R8G8B8` (`21`) | `D3DPOOL_DEFAULT` (`0`) | `D3DUSAGE_DYNAMIC` (`0x200`) | 3 |
| `CreateTexture` | `D3DFMT_L8` (`50`) | `D3DPOOL_DEFAULT` (`0`) | `D3DUSAGE_DYNAMIC` (`0x200`) | 1 |
| `CreateTexture` | `D3DFMT_A8L8` (`51`) | `D3DPOOL_DEFAULT` (`0`) | `D3DUSAGE_DYNAMIC` (`0x200`) | 1 |
| `CreateVertexBuffer` | `D3DFMT_VERTEXDATA` (`100`) | `D3DPOOL_DEFAULT` (`0`) | `D3DUSAGE_WRITEONLY` (`0x8`) | 1 |
| `CreateVertexBuffer` | `D3DFMT_VERTEXDATA` (`100`) | `D3DPOOL_DEFAULT` (`0`) | `D3DUSAGE_DYNAMIC \| WRITEONLY` (`0x208`) | 1 |
| `CreateIndexBuffer` | `D3DFMT_INDEX16` (`101`) | `D3DPOOL_DEFAULT` (`0`) | `D3DUSAGE_DYNAMIC \| WRITEONLY` (`0x208`) | 2 |

`IDirect3D9::CheckDeviceFormat` probes (`225` total calls):

| Resource type | Usage | Format | Calls |
| --- | --- | --- | ---: |
| `D3DRTYPE_SURFACE` (`1`) | `D3DUSAGE_DEPTHSTENCIL` (`0x2`) | `D3DFMT_D32` (`71`), `D15S1` (`73`), `D24S8` (`75`), `D24X8` (`77`), `D24X4S4` (`79`), `D16` (`80`) | 36 each (`216`) |
| `D3DRTYPE_TEXTURE` (`3`) | `D3DUSAGE_RENDERTARGET` (`0x1`) | `D3DFMT_R16F` (`111`), `D3DFMT_R32F` (`114`) | 2 each (`4`) |
| `D3DRTYPE_TEXTURE` (`3`) | `D3DUSAGE_RENDERTARGET \| QUERY_POSTPIXELSHADER_BLENDING` (`0x80001`) | `D3DFMT_A16B16G16R16F` (`113`) | 2 |
| `D3DRTYPE_TEXTURE` (`3`) | `D3DUSAGE_QUERY_FILTER` (`0x20000`) | `D3DFMT_R32F` (`114`) | 2 |
| `D3DRTYPE_TEXTURE` (`3`) | `D3DUSAGE_DEPTHSTENCIL` (`0x2`) | `D3DFMT_D24S8` (`75`) | 1 |

### Effect surface summary

- **Effects loaded:** `2` (`D3DXCreateEffectFromFileA`: `mb_2a.fxo` / `postFX.fxo` or module effect).
- **Techniques queried at startup:** `480` distinct technique names (`96` base techniques × `5`
  variants: base, `_HDR`, `_PRESHADED`, `_SHDW`, `_SHDWNVIDIA`, plus instanced variants for
  `diffuse_dynamic`, `envmap_specular_diffuse`, `flora`, and `standart_noskin_*`).
- **Parameters queried at startup:** `164` distinct names (`65` bone-palette entries
  `matWorldArray` + `matWorldArray[0..63]`, `12` texture samplers, `9` view/proj/shadow/water/motion
  matrices, `16` feature booleans, `27` scalar/offset floats & flags, `3` ints, `32`
  lighting/fog/weather/blood/water vectors).
- **Per-frame render cadence (main-menu loop):** `1` `Clear(TARGET | ZBUFFER)`, `1` `BeginScene`,
  `2` `SetViewport`, `2` `ID3DXEffect::Begin`/`End` passes, `5` `CommitChanges` +
  `DrawIndexedPrimitive(D3DPT_TRIANGLELIST)` + `SetStreamSource`, `1` `VertexBuffer::Lock/Unlock`,
  `1` `EndScene`, `1` `Present`, and `~20` `IDirect3DStateBlock9::Apply` calls.

## Android performance (decided with the user, 2026-11-21)

When the game runs (R5 onward), performance is part of "done" for every slice, measured on the
phone rather than deferred. Each optimization gets a before-and-after measurement.

**Targets (decided with the user, 2026-11-21):**
- **Tiers go by chip and GPU, not RAM.** The user's phone (OPPO Reno 11) has 12 GB RAM but a
  mid-range Mali-G68: RAM alone put it in the flagship tier, its GPU in the mid-range one.
- **Device floor:** `arm64-v8a` Android 10+ with ARMv8.2-A (LSE + RCpc), Vulkan and at least 6 GB RAM
  (Mali-G68/PanVK, mid-range Adreno/Turnip, and System Vulkan). Out of scope: pre-ARMv8.2-A CPUs
  (without LSE/RCpc) or under 6 GB RAM.
- **Mid-range GPU tier (reference phone: OPPO Reno 11, 12 GB, Mali-G68):** **30 FPS sustained for
  30+ minutes** in 100-troop battles at ~720p / 85% render scale, and **60 FPS** in menus and on the
  campaign map.
- **Flagship GPU tier (Snapdragon 8 Gen 2+ / Dimensity 9000+ class):** playable **1,000+ troop WSE2
  battles** at ≥ 30 FPS sustained (R9), and **90 Hz / 120 Hz sustained** in menus, campaign map, and
  mid-size battles (R11; decided with the user, 2026-11-22).
- **Every supported phone:** 1,000+ troop battles load and run with zero OOM and zero fixed-size GPU
  buffer/descriptor overflows; on the reference phone their frame rate is measured and recorded, not
  gated. Designing for 1,000+ agents from R5 onward avoids hard bottlenecks later:
  - *GPU submission (R5):* growable ring-buffered dynamic vertex/index buffers (`D3DLOCK_DISCARD` /
    `D3DLOCK_NOOVERWRITE` without pipeline stalls), dynamic-offset uniform ring buffers sized for
    thousands of draws and bone-palette uploads per frame, hardware instancing (`SetStreamSourceFreq`),
    and zero per-draw descriptor allocations.
  - *Audio voice contention (R6):* strict voice caps and priority eviction in `WseAudioMixer` so
    hundreds of simultaneous melee/voice triggers never stall the game thread.
  - *CPU simulation throughput (R9):* Bullet physics, agent AI, pathfinding, and bone evaluation
    dominate at 1,000+ troops — making R3 TSO relaxation on thread-local paths, direct cross-batch
    call linking, and `-O3` / big-core tuning benchmarks first-class requirements rather than optional
    polish.

**Codegen target (resolves the `Preparing on a PC` vs host-CPU split, decided with the user, 2026-11-21):**
- Both PC-prepared builds and on-phone builds keep `portable_android_target()` (`generic`,
  `+v8.2a,+rcpc` — LSE atomics for `lock`, `LDAPR` for acquire loads) at `-O2` by default so PC zips
  and on-phone batches share one cache key.
- In R9, benchmark big-core CPU scheduling (e.g. `cortex-a78` / `cortex-x1`) against `generic` on
  the phone; add an optional on-phone "Optimize for this device" mode only if measured battle FPS
  improves by >10%.

**To measure and do from R5 onward:**
- **Dispatch.** Count indirect calls, returns, and cross-batch calls through `Runtime::dispatch` per
  frame; link known direct targets across batches where profiling shows hot call edges.
- **Code quality & translation time.** Time hot functions at `-O2` vs `-O3` (and cold functions at
  `-O1` to cut on-phone translation time without losing battle FPS).
- **Memory ordering (R3).** Measure the cost of acquire/release (`LDAPR`/`STLR`) and misaligned
  barriers on hot gameplay paths; relax only memory proven single-thread-owned.
- **Graphics (R5), from the 1,000-frame "R5 census".** Every frame runs through `ID3DXEffect` and
  state blocks (`~20` `StateBlock::Apply`, `~60` effect constant sets, `5` draws on the main menu):
  batch constants into one uniform ring buffer per draw, keep a persistent `VkPipelineCache`,
  compile shaders ahead of time into a disk cache, allocate zero descriptors per draw, and use
  tile-friendly render passes (clear / don't-care load and store, zero mid-frame readbacks) for Mali
  and Adreno.
- **Textures.** `702/705` startup textures are DXT1/3/5. Use native Vulkan `textureCompressionBC`
  where the driver supports it (PanVK/Turnip); otherwise transcode once to cached ETC2/ASTC on disk
  so mobile GPUs never pay 4–8× RGBA8 VRAM/bandwidth on every load.
- **Frame pacing and heat.** Android ADPF performance-hint sessions (`perf_hint.cpp`) for the game
  and render threads, big-core affinity, paced presents (`FramePacer`), and a 30-minute sustained
  battle run on the phone (measuring frame times at minute 30, not minute 1).
- **Measuring.** Extend the FPS overlay into a per-frame CPU/GPU frame-time and thermal-tier view
  (`docs/PLATFORM_PLAN.md` "P14").

## Post-R4 slice designs (R5–R12, decided with the user, 2026-11-21 / 2026-11-22)

### R5 — Graphics (D3D9 + D3DX → Vulkan)

- **Goal:** Replace the R4 recording stub (`shim_d3d9.h`) with a Vulkan-backed D3D9/D3DX
  implementation that renders the base forward pipeline (UI, campaign map, scenes, skinned battles,
  basic shadows) at mobile-friendly frame times.
- **Sub-slices:**
  - **R5a (System Vulkan + P5 shader bridge + base draws):** Wire `shim_d3d9.h` onto `VulkanRhiDevice`
    using System Vulkan (`VK_KHR_android_surface` swapchain) first to minimize variables. Implement
    `IDirect3DStateBlock9` capture/apply, growable dynamic vertex/index and effect constant ring
    buffers (`D3DLOCK_DISCARD`/`NOOVERWRITE`, sized to scale to 1,000+ troop draw/bone-palette
    counts without pipeline stalls or per-draw descriptor allocations), `SetStreamSourceFreq`
    instancing, persistent `VkPipelineCache`, and map `ID3DXEffect` technique/parameter names (`480`
    queried techniques, `164` parameters in the R5 census) onto the existing P5 `GLShaders` → SPIR-V
    compiler (`wse_shader` + `SpirvShaderCache`). Decode DXT textures via `dds.h` with native BC
    when `textureCompressionBC` is present, or temporary RGBA8 + P10.7 mip-budget capping during R5a
    bring-up. Advertise HDR (`A16B16G16R16F`), DOF, and MSAA (`CheckDeviceMultiSampleType`, `666`
    probes) as unavailable via `CheckDeviceFormat`/`CheckDeviceMultiSampleType` until R11.
  - **R5b (Cached ETC2/ASTC transcoding + `fx_2_0` SM2/3 → SPIR-V):** Add one-time DXT1/3/5 →
    ETC2/ASTC transcoding cached on disk (replacing the temporary RGBA8 fallback on drivers without
    native BC) and add a `fx_2_0` (`.fxo`) parser + SM2/3 shader-bytecode → SPIR-V translator so
    custom mod `.fxo` effects run at PC parity.
  - **R5c (PanVK / Turnip custom-ICD presentation):** Wire the `offscreen_hardware_buffer_bridge`
    (`AHardwareBuffer` + `ASurfaceControl`, `docs/PLATFORM_PLAN.md`) so direct-loaded PanVK (Mali-G68)
    and Turnip (Adreno) present frames cleanly alongside System Vulkan.
- **Depends on:** R4 green on phone; P4 `VulkanRhiDevice` GPU path; P5 `wse_shader` +
  `SpirvShaderCache`.
- **Done when:**
  - Host synthetic D3D9→Vulkan tests pass in CI (`host-linux` + `android-arm64`).
  - On the user's phone (System Vulkan in R5a, PanVK in R5c), `Native` renders the main menu,
    campaign map, and a quick battle with correct textures, skinning, alpha-test/blend, and zero
    validation/fault crashes; main menu holds **60 FPS** and steady-state shader/texture caches hit
    with zero mid-frame recompiles.
- **Risks:**
  - `ID3DXEffect` technique/parameter naming differences between `.fxo` and `GLShaders` XML pairs
    (mitigated by R5a's census list of 480 techniques + 164 parameters and R5b's `.fxo` SM2/3 path).
  - `AHardwareBuffer` sync-fence / swapchain latency quirks across PanVK and Turnip versions
    (mitigated by proving System Vulkan first in R5a and isolating custom-ICD presentation in R5c).
  - VRAM spikes on 6 GB phones if RGBA8 fallback runs on large battles (mitigated by P10.7 mip-cap
    in R5a and replacing RGBA8 with cached ETC2/ASTC in R5b).

### R6 — Audio, Input & Touch Controls

- **Goal:** Give the recompiled game full audio playback and first-class mobile controls (touch,
  gamepad, keyboard/mouse, soft keyboard).
- **Design:**
  - **Audio:** Shim the 54 delay-loaded `fmodex64.dll` exports (`FMOD::System`, `Sound`, `Channel`,
    `ChannelGroup`, 2D/3D positions, looping, volume/pitch, streaming) directly onto P6's
    `WseAudioMixer` + `AudioMixerWorker` + `SDL_OpenAudioDeviceStream` (`wse_audio` WAV/OGG/MP3
    decoders). No `fmodex64.dll` translation and no `WINMM`/`MSACM32` audio shims are required.
  - **Input:** Feed `USER32` (`WndProc`), `DINPUT8` (`GetDeviceState`/`GetDeviceData`), and
    `XINPUT1_3` from SDL3 keyboard, mouse, touch, and gamepad events.
  - **Touch overlay & QoL:** Draw the P11/P14 touch overlay as a final Vulkan pass inside
    `IDirect3DDevice9::Present()` (works identically on System Vulkan and custom-ICD
    `AHardwareBuffer` with exact touch coordinates). Auto-switch control scenes (Battle, Campaign
    Map, Trade/Inventory, Party, Dialog, Menu) via a hybrid detector: `ShowCursor` / pointer-capture
    as the universal menu-vs-3D fallback, refined by reading `g_gameScreen` (via matching PDB symbol
    lookup) when available, plus a small corner manual override button. Full touch QoL layout,
    buttons, soft-keyboard `WM_CHAR` injection, auto-pause on background, and layout editor specs
    live in `docs/PLATFORM_PLAN.md` ("P14").
  - **Note on `lua5164`:** Already translated and verified in R4; R6 verifies Lua scripts stay
    working under live input and audio.
- **Depends on:** R5a (for Vulkan overlay pass in `Present()`); P6 `WseAudioMixer`; P7/P11 input &
  touch layout primitives.
- **Done when:**
  - Host tests cover the FMOD-API shim (2D/3D playback, streaming, pause/resume) and DInput/XInput/
    `WM_CHAR` event injection.
  - On the phone, `Native` plays music and 3D battle sound effects cleanly (pausing on background and
    resuming without glitches), all per-scene touch controls appear only in their matching scene,
    soft keyboard enters character/save names, and attaching a Bluetooth/USB gamepad hides the touch
    overlay automatically.
- **Risks:**
  - A mod or future WSE2 build calling an unshimmed FMOD DSP/reverb export (mitigated by logging
    unsupported optional DSP calls as clean no-ops and faulting loudly only on unshimmed lifecycle
    exports during testing).
  - PDB-less future builds or custom mod presentations confusing screen detection (mitigated by
    `ShowCursor` fallback + corner manual override pill).

### R7 — WSL x86-64 Bring-Up & Savegame Oracle

- **Goal:** Debug full gameplay (main menu → campaign map → town/tavern scene → battle → save/load)
  on x86-64 under `gdb` in WSL Ubuntu before fighting mobile GPU/thermal variables, and prove
  byte-compatible savegames against retail Windows Warband.
- **Design:** Resolves the earlier "on Windows" statement in `Slices`: R7 runs in **WSL Ubuntu
  x86-64** (reusing R4's `tools/r4_run.cpp` LLVM 18 toolchain) with our shims only (R5 Vulkan + R6
  SDL3 audio/input). Interactive play uses WSLg Vulkan/SDL3 where available, with Mesa Lavapipe
  software Vulkan + frame-dump / scripted-input verification as the deterministic fallback.
- **Depends on:** R5a/R5b, R6.
- **Done when:**
  - Translated `Native` runs in WSL from main menu → new character creation → campaign map → entering
    a town scene → fighting a battle → saving and reloading, with zero faults and zero missing
    imports.
  - **Savegame oracle passes:** a save created by retail Windows WSE2 loads cleanly in translated
    WSL WSE2, and a save written by translated WSL WSE2 loads cleanly in retail Windows WSE2 (and
    passes our Phase 2A `savegame.h` structural check).
- **Risks:**
  - Gameplay-only Windows imports (never hit in R4's 1,000-frame startup loop) surfacing during
    character creation, battle physics, or saving (mitigated by fixing them under `gdb` in WSL before
    R8).
  - WSLg GPU/Vulkan driver limitations on Intel HD 520 (mitigated by Lavapipe fallback for
    frame-dump/correctness verification).

### R8 — Phone Bring-Up & Playable v1 (`Native`)

- **Goal:** Deliver playable v1 `Native` end-to-end on the user's phone with reliable first-time
  translation, permanent save storage, and sustained performance.
- **Design:**
  - **Fix on-phone translation reliability & speed:** R4 measured ~163 s/batch (~3.5–4+ h total) and
    stalled at `77/80` on the 3 largest batches (each 3–3.5× average size), most likely out of memory:
    a PC run in WSL (3.8 GB) hung on the same batches the same night. **Pulled forward (2026-11-21):**
    the memory fix (largest batches first, split oversized batches, a per-thread memory budget) lands
    in the next native update, before R5, because every native update until R8 retranslates on the
    phone and on the PC. Split oversized
    batches by instruction/function cap (instead of fixed 64 KiB windows alone) so peak worker RSS
    stays bounded, enforce per-worker memory budgets, and benchmark `-O1` on cold batches (functions
    outside hot startup/battle paths) to cut first-time phone translation time without hurting battle
    FPS. Keep PC preparation (`--for-phone` zip import) working as an optional fast path sharing the
    same `portable_android_target()` cache key.
  - **Permanent user storage & Save Manager:** Migrate the writable layer from `filesDir/recomp/r4/user/`
    to permanent `filesDir/wse2/user/` (with one-time migration of existing `rgl_config.txt` / saves),
    wired to the Launcher Save Manager (`docs/PLATFORM_PLAN.md` "P14") for SAF/Downloads `.sav`
    export and import.
  - **Launcher Play path:** Wire the launcher's main `Play` button directly to the `:recomp`
    recompiled game runtime (replacing the P9 clean-room preview shell as the primary launch path).
- **Depends on:** R5c, R6, R7, and `docs/PLATFORM_PLAN.md` "P14".
- **Done when:**
  - A fresh on-phone translation of `mb_warband_wse2_x64.exe` + `lua5164.dll` finishes end-to-end on
    the user's phone with zero manual intervention and zero OOM stalls, AND importing a PC-prepared
    zip skips on-phone translation cleanly.
  - On OPPO Reno 11 (Mali-G68, PanVK and System Vulkan), `Native` is playable end-to-end: main menu,
    character creation, campaign map, scenes, 100-troop battle, saving/reloading, and exporting a
    `.sav` that loads in retail PC Warband.
  - **Measured sustained performance:** **≥ 30 FPS sustained at minute 30** of a 100-troop battle at
    ~720p / 85% render scale (frame-time HUD + thermal log recorded), and **60 FPS** in menus and on
    the campaign map.
- **Risks:**
  - Thermal throttling past minute 15–20 on Mali-G68 (mitigated by R5b ETC2/ASTC texture cache, ADPF
    hint sessions, and P8 hysteretic thermal scaling).
  - Uncovered ARM64-only weak-memory or alignment bugs not seen on x86-64 WSL (mitigated by R3's
    conservative TSO ordering remaining on until R9 proves safe relaxations).

### R9 — Performance, Hardening & Mods (PC Parity)

- **Goal:** Push translated ARM64 CPU and Vulkan GPU throughput close to native for 1,000+ troop
  battles, harden crash reporting, and verify PC-parity support for heavy WSE2 mods.
- **Design (expanded with the user, 2026-11-22):**
  - **Register-passing native ARM64 ABI & cross-batch inlining/ThinLTO:** Stop spilling Win64
    argument and return registers (`rcx/rdx/r8/r9/rax/xmm0-3`) through `CpuState*` on every direct
    call between translated functions: pass them directly in ARM64 hardware registers (`x0–x7`,
    `v0–v7`) for direct calls, keep thin `CpuState*` thunks only for indirect dispatch / SEH entry,
    and enable cross-batch direct linking + inlining (or ThinLTO) for tiny hot leaf helpers (vector,
    matrix, string, and Bullet math).
  - **On-device Profile-Guided Optimization (PGO):** Add a lightweight profiling run mode that
    samples a 2-minute real battle/campaign session to rank the top ~5% hottest functions (Bullet
    physics, agent AI, pathfinding, skinning, script VM), then re-translates only those hot
    functions at `-O3` with big-core CPU scheduling (`cortex-a78` / `cortex-x3` / `cortex-x4`) while
    keeping the cold ~95% at `-O1` (cuts total translation time while raising sustained battle FPS).
  - **Selective TSO relaxation & 128-bit SSE→NEON vectorization:** Measure R3 `LDAPR`/`STLR` and
    misaligned-barrier overhead on hot gameplay loops; relax ordering only on memory proven
    single-thread-owned (validated under multi-threaded battle stress runs) and keep packed 128-bit
    SSE arithmetic/shuffles lowered directly to ARM64 NEON vectors without unnecessary scalarization.
  - **Indirect / instanced draw batching in D3D9→Vulkan:** Detect repeated mesh/material draws per
    frame (flora, arrows, shields, identical troop equipment) in the D3D9→Vulkan shim and merge them
    into instanced / indirect Vulkan draws to cut CPU draw-call overhead in 1,000+ troop battles.
  - **Crash & diagnostic reporting:** Any unhandled fault, unknown indirect target VA, or unshimmed
    import writes a shareable PDB-symbolized crash report (guest VA, symbol name if present,
    batch/module, Android device + Vulkan driver info) viewable in the launcher and modder console
    (`docs/PLATFORM_PLAN.md` "P15").
  - **PC-parity mod support:** Full parity with PC WSE2 mods — Module System scripts, WSE2 Lua,
    custom `.fxo` shaders (R5b), and multi-GB `.dds` texture packs kept within a 6 GB phone's RAM via
    on-demand textures, mip-cap, and cached ETC2/ASTC. If a mod attempts `LoadLibrary` on an extra
    third-party x64 Windows DLL, surface a clear diagnostic (or translate through `shim::Modules` if
    a supported mod requires it).
- **Depends on:** R8.
- **Done when:**
  - `Native`, `Phantasy Calradia`, and `FiveDynastiesAndTenKingdoms` run end-to-end on the phone
    (main menu, campaign map, battle, save/load, WSE2 Lua features, custom shaders) without OOM
    kills on 6 GB phones and within the v1 sustained FPS target on `Native` (with documented timings
    for heavy mods).
  - **1,000+ troop stress benchmark:** a 1,000-troop WSE2 battle runs with zero GPU buffer/descriptor
    overflows, zero audio dropouts, and zero crashes on every supported phone, sustaining **≥ 30 FPS**
    on the flagship GPU tier (with full CPU/GPU frame-time breakdown recorded).
  - Before/after phone frame-time logs are recorded in `STATUS.md` for every compiler and GPU
    optimization kept (register ABI, PGO, TSO relaxation, NEON vectorization, draw batching).
- **Risks:**
  - Register-passing ABI interacting with Win64 SEH frame unwinding or indirect calls (mitigated by
    keeping the Win64 guest stack frame layout untouched, using `CpuState*` entry thunks for
    indirect targets, and verifying every SEH corpus case on CI x86 + arm64 before touching retail).
  - TSO relaxation introducing rare ARM64 weak-memory races (mitigated by relaxing only proven
    single-thread loops and running multi-hour battle stress tests with a toggle to revert to strict
    TSO).
  - Large mod texture packs exhausting storage or RAM during ETC2/ASTC transcoding (mitigated by
    streaming transcode with bounded disk cache and P10.7 mip cap).

### R10 — WSE2 Updates & Player Distribution

- **Goal:** Make the app clean to update across new WSE2 releases and simple for other players to
  install and prepare without developer tools.
- **Design:**
  - **WSE2 release resilience & cache hygiene:** Accept any x64 WSE2 build whose shipped PDB matches
    (no exe hash pin); keep v1.1.5.6 as the CI/evidence baseline until an explicit rebase
    (`tools/rebase_build.py`). Whenever the exe hash or `translator_fingerprint()` changes,
    automatically prune stale `.so`/`.key` batches from app storage so old caches do not consume
    gigabytes. If a user supplies an exe with a missing/mismatched PDB or a newer WSE2 build calls an
    unshimmed import, show a plain-language error naming the missing PDB or DLL!import.
  - **Release signing & GitHub Releases (no Google Play):** Ship signed release APKs on GitHub
    Releases using a separate private release keystore (never `android/debug.keystore`, `AGENTS.md`
    §3). Google Play Store distribution is explicitly out of scope (decided with the user,
    2026-11-22).
  - **Standalone PC prep tool:** Package a prebuilt standalone Windows + Linux PC preparation CLI zip
    alongside each GitHub Release APK so players who want fast PC translation can run it directly
    without installing WSL, clang, CMake, or LLVM.
- **Depends on:** R8 (v1 release) and R9 (mod-hardened release).
- **Done when:**
  - Switching between two WSE2 builds or upgrading the APK cleanly prunes stale batches and
    retranslates only what changed.
  - A fresh user on a non-developer Windows PC and an Android 10+ phone at the device floor can install
    the release APK from GitHub Releases, optionally run the standalone PC prep zip (no WSL), and play
    `Native` from start to battle.
- **Risks:**
  - Future WSE2 releases adding new Windows/D3D9 imports (mitigated by clear import-name diagnostics
    and `translator_fingerprint()` keeping shim-only APK updates from forcing retranslation).

### R11 — Full PC Visual Parity, Modern Mobile Graphics & High Refresh Rate (decided with the user, 2026-11-22)

- **Goal:** Close the remaining D3D9 visual gaps from R5a and add modern mobile-native graphics
  features so Warband looks as sharp or sharper than PC while holding high frame rates on flagship
  phones.
- **Design:**
  - **Full PC D3D9 visual parity:** Enable real-time shadow maps (`R16F`/`R32F` depth/shadow render
    targets with PCF filtering), HDR post-processing and auto-exposure (`A16B16G16R16F` render
    targets + `postFX.fxo`), Depth of Field, and planar water reflections (advertising them via
    `CheckDeviceFormat` when enabled in Settings).
  - **Modern mobile upscaling & post-process AA:** Add an optional Vulkan-native spatial upscaler +
    sharpening pass (FSR 1.0 / CAS) so 75–85% render scale stays crisp at native 1080p/1440p, plus
    lightweight post-process anti-aliasing (FXAA / SMAA) as a tile-GPU-friendly alternative to heavy
    MSAA.
  - **High-refresh-rate gameplay (90 Hz / 120 Hz):** Tune frame pacing (`ANativeWindow_setFrameRate*`,
    `FramePacer`, mailbox/FIFO present modes) to sustain 90 Hz / 120 Hz on flagship GPUs in menus,
    campaign map, and mid-size battles.
  - **Android HDR10 / Wide-Color output:** When the display supports `VK_COLOR_SPACE_HDR10_ST2084_EXT`
    (or `BT709`/DCI-P3 wide color) and HDR postFX is enabled, present directly to an HDR10 Vulkan
    swapchain with proper PQ tone-mapping.
- **Depends on:** R5b/R5c, R8, R9.
- **Done when:**
  - On-device verification shows working shadow maps, HDR auto-exposure, DOF, water reflections,
    FSR 1.0/CAS upscaling, and FXAA/SMAA with toggles in Settings and the live in-game drawer (`P15`).
  - Flagship GPU tier sustains **90–120 FPS** in menus/campaign map and mid-size battles; HDR10
    output verifies on an HDR-capable display without clipping or black-crush.
- **Risks:**
  - Multiple offscreen render targets (`A16B16G16R16F`, shadow maps, water reflection pass) hurting
    tile-GPU bandwidth on mid-range Mali-G68 (mitigated by making every R11 pass independently
    toggleable with conservative presets for the mid-range tier).
  - `AHardwareBuffer` HDR10 dataspace plumbing across PanVK/Turnip custom ICDs (mitigated by
    gating HDR10 output on runtime swapchain/dataspace capability probes, falling back cleanly to
    SDR).

### R12 — Native Multiplayer: LAN, Direct-IP & WSE2 Community Servers (decided with the user, 2026-11-22)

- **Goal:** Bring Warband & WSE2 multiplayer to Android over Wi-Fi LAN, direct-IP connections, and
  non-Steam WSE2 community servers while remaining 100% BYOA and clean-room.
- **Design:**
  - **Real `WS2_32` over POSIX/BSD sockets:** Replace the R4 offline `shim_net.h` stubs with real
    non-blocking UDP/TCP BSD sockets (`WSAStartup`, `socket`, `bind`, `connect`, `sendto`, `recvfrom`,
    `select`/`WSAPoll`, `ioctlsocket`, `setsockopt`, `getaddrinfo`, proper `WSAGetLastError`
    mapping) supporting Warband's UDP game protocol and LAN broadcast discovery.
  - **Real `WINHTTP` / `WININET` over Android TLS:** Route WSE2 server-list / master-server HTTP(S)
    queries through Android's native TLS stack (or JNI `HttpURLConnection`) so non-Steam WSE2
    community server lists populate cleanly.
  - **Clean-room boundary:** `steam_api64` remains stubbed; no Steam DRM circumvention or fake
    Steam tickets. Only standard Warband/WSE2 LAN, direct-IP, and non-Steam community servers are
    supported.
- **Depends on:** R8 (v1 single-player playable).
- **Done when:**
  - Host tests verify `WS2_32` UDP/TCP loopback, non-blocking `select`, and `WSAGetLastError`
    semantics against Windows behavior.
  - On the phone, two devices (or a phone + PC running retail WSE2) connect over Wi-Fi LAN and
    direct-IP, join a multiplayer battle, and play to round end without packet-loss disconnects or
    desyncs; non-Steam WSE2 server lists load cleanly.
- **Risks:**
  - Android Doze/background socket throttling or multicast/broadcast permission quirks on some Wi-Fi
    routers (mitigated by holding `WifiManager.MulticastLock` only while on the LAN browser screen
    and supporting direct-IP entry).
  - Windows `SOCKET` handle values vs POSIX fd numbers if guest code mixes `WS2_32` and `KERNEL32`
    calls (mitigated by isolating socket handles cleanly in `shim_net.h`).

## Out of scope (decided with the user, 2026-11-21 / 2026-11-22)

- **Google Play Store distribution:** GitHub Releases is the sole official distribution channel;
  no Google Play Store submission or AAB/Play-policy compromises.
- **Steam DRM / Steam-authenticated multiplayer:** `steam_api64` stays stubbed (`AGENTS.md` §1 BYOA
  and no-DRM-bypass rule).
- **Weaker / older phones:** devices under 6 GB RAM or pre-ARMv8.2-A CPUs lacking LSE/RCpc.

## What carries over

- P-slices: the launcher (P10/P12/P13/P14/P15), SAF `Vfs`, the Vulkan RHI and GPU path (P4), shader
  translation (P5), the audio mixer (P6), input and the touch overlay (P7/P11/P14/P15), and the
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
| Android refuses to run generated code | Resolved in R0 (a): `memfd` (`android_dlopen_ext`) with our own ELF loader (`code_loader.h`) as fallback. |
| On-phone translation OOM / 3.5–4 h runtime | Stalled at 77/80 on the 3 largest batches (memory, most likely; WSL hung on the same batches); the next native update translates the largest batches first, splits oversized batches by instruction/function cap and bounds worker RSS; R8 tests `-O1` on cold batches, and keeps standalone PC prep (R10) as a fast path. |
| C++ exceptions and SEH | Built in R3 and tested on x86/arm64 CI; R7 verifies live retail throw paths under WSL `gdb`. |
| D3DX `fx_2_0` effect bytecode vs `GLShaders` | R5a bridges `ID3DXEffect` onto the verified P5 `GLShaders` → SPIR-V path first; R5b adds offline-tested `fx_2_0` SM2/3 → SPIR-V for custom mod effects. |
| DXT1/3/5 VRAM & bandwidth on mobile GPUs | R5a uses native Vulkan `textureCompressionBC` where present (else temporary RGBA8 + mip cap); R5b caches one-time ETC2/ASTC transcodes on disk. |
| Custom-ICD (`PanVK`/`Turnip`) presentation sync | R5a proves D3D9→Vulkan on System Vulkan (`VkSurfaceKHR`) first; R5c isolates `AHardwareBuffer` + `ASurfaceControl` fencing. |
| Sustained 30-min thermal throttling on mid-range SoCs | R8 gates "done" on a 30-minute 100-troop battle log with ADPF, constant-buffer batching, persistent `VkPipelineCache`, and P8 thermal scaling. |
| Weak-memory bugs that single-threaded tests miss | Conservative R3 TSO ordering stays on by default; R9 relaxes only proven thread-local paths after multi-threaded phone stress runs. |
| Newer WSE2 release or missing/mismatched PDB | Accepts any x64 WSE2 build with a matching PDB (no exe hash pin); auto-prunes stale `.so` caches on change and reports missing PDBs or new unshimmed imports plainly (R10). |
