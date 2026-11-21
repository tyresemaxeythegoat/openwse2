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
| **R4 OS shims** | All 546 imports listed with an owner; the startup path (`WinMain`, window, configuration, file I/O) runs to the main loop on a recording stub D3D9 device, with `lua5164` translated (see "R4 design"). |
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

**Phone run (decided with the user, 2026-11-21).** Advanced > "R4 startup run" runs the same
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
- **Loading.** Batches load from a `memfd` (`android_dlopen_ext`), else with our own ELF loader
  (`code_loader.h`, shared with the R0 spikes): on the phone `dlopen` of a read-only app-data file
  failed in R0 run 2. Guest images are mapped read-write only (translated code runs, guest bytes
  never execute).

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
