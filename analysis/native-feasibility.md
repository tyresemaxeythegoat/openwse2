# Native feasibility dossier

Target: WSE2 x64 (`mb_warband_wse2_x64.exe`, PE32+ x86-64) running natively on
Android ARM64 by ahead-of-time static recompilation.

Explicit constraints:

- No MNBTool engine mod.
- Native execution, not emulation: x86-64 semantics resolved at build time.
- Android static-recomp pipeline, not a compatibility layer.
- No game data in the repository or APK.

## 1. Why static recompilation is viable

The engine is a single PE32+ image with complete function metadata:

| Property | Value |
|---|---|
| Image base | `0x140000000` |
| Entry | `0x140481CE0` |
| `.text` virtual size | 5,125,605 B |
| `.pdata` extents | 14,461 usable (`14,464` raw, 3 degenerate) |
| Primary functions | 9,202 |
| CHAININFO fragments | 5,259 |
| Code coverage, all extents | 94.07% |
| Code coverage, primary extents | 73.52% |
| `.text` with no `.pdata` | 5.93% (303,711 B) |
| Public symbols | 22,600 |
| Instructions in extents | 1,151,840 |

`.pdata` gives exact start and end for 9,202 primary functions and 5,259 cold
fragments. That removes function boundary discovery for most of the image, but
not all of it: 5.93% of `.text` is leaf code and thunks with no `.pdata`
record, and a heuristic scan finds 1,164 unique direct-call targets in those
gaps. The lifter must discover those ranges before it can resolve every
callee. The shipped PDB supplies names for 7,086 of the 14,461 starts; the
rest are anonymous but bounded.

Static recompilation produces C, then native AArch64. There is no instruction
fetch, no decode, and no interpreter loop at runtime. The differentials in
`analysis/proof-results.md` demonstrate semantic equivalence on real engine
code, and the ARM64 objects disassemble to ordinary AArch64.

## 2. Instruction surface

Per-extent scan of 1,151,840 instructions:

| Feature | Count | Share |
|---|---|---|
| Vector register operands (`xmm`/`ymm`) | 184,227 | 15.99% |
| Conditional branches | 107,410 | 9.33% |
| Direct calls | 68,618 | 5.96% |
| RIP-relative memory | 67,136 | 5.83% |
| Direct jumps | 25,337 | 2.20% |
| Indirect calls | 14,771 | 1.28% |
| `cmov` | 3,975 | 0.35% |
| `setcc` | 2,161 | 0.19% |
| Traps (`int3`, `ud2`, `hlt`) | 1,201 | 0.10% |
| Indirect jumps | 419 | 0.04% |
| `div`/`idiv` | 329 | 0.03% |
| Locked operations | 327 | 0.03% |
| High-8 registers (`ah`..`dh`) | 239 | 0.02% |
| `rep`-prefixed | 157 | 0.01% |
| `cpuid`/`rdtsc` | 11 | ~0 |
| x87 | 10 | ~0 |
| Segment overrides | 19 | ~0 |

Consequences for the lifter:

- SSE2 is mandatory, but x87 is effectively absent. Ten instructions total can
  be handled as one-off fixups.
- `gs` appears twice. TLS access is a small, enumerable shim.
- Indirect calls are the main dispatch cost: 14,771 sites, all resolved through
  a target table at runtime.
- Locked operations are rare (327), but that does not make the memory model
  free. x86-64 gives strong ordering; ARM64 does not. The current lift uses
  plain C loads and stores, so a host mutex around each guest lock is not
  enough to preserve publication/refcount ordering. This is an explicit
  runtime gap, described in section 3.

## 3. Runtime architecture

```
+--------------------------------------------------------------+
| Android app (NativeActivity, SDL2)                           |
|                                                              |
|  +--------------------- guest address space --------------+  |
|  |  image at 0x140000000  (r/o, from user's install)      |  |
|  |  heap / stack / TLS / shim arenas  (r/w)               |  |
|  +--------------------------------------------------------+  |
|                                                              |
|  CPU state: r[16], flags, rip, xmm[16]                       |
|                                                              |
|  f_<rva>(CPU*)   ... 14.5k generated native functions        |
|                                                              |
|  import thunks -> Win32 / D3D9 / DInput / FMOD / Lua shims   |
+--------------------------------------------------------------+
```

### Memory ordering (explicit gap)

The lifted C model is x86-shaped: plain `ld*`/`st*` accesses with no fences.
That is correct for the single-threaded differential rigs, but x86-64 is
strongly ordered (TSO) and ARM64 is weakly ordered. Once engine threads share
state, plain accesses can be observed out of order on the phone even though
the same sequence is correct on the x64 host.

Consequences:

- Guest `lock` operations must lower to C11 `atomic_*` operations or
  `__atomic_*` builtins with at least acquire/release semantics, not to a
  plain read-modify-write plus a host mutex.
- Refcounts, work queues, input state, and resource publication need
  acquire/release at the handoff points. A mutex only protects the critical
  section; it does not order non-atomic accesses outside it.
- The runtime should expose `wse2_load_acquire` / `wse2_store_release`
  helpers (or a per-access annotation in the lifter) so generated code can be
  audited. A conservative `__atomic_thread_fence(__ATOMIC_SEQ_CST)` at shim
  and thread boundaries is the first bring-up step, not the final model.
- Single-thread differential tests cannot catch this class of bug. It needs a
  multi-threaded stress mode on a device or an emulator with a weak memory
  model.

This is deliberately deferred until the bulk lift and threading shims exist;
it is tracked here so the x64-only proofs are not mistaken for a complete
memory model.

### Code size and performance

The current emitter is correctness-first and does not optimize. Measured
AArch64 text sizes for the proof functions:

| Function | x86-64 | AArch64 | Ratio |
|---|---:|---:|---:|
| `memcmp` | 199 B | 2,640 B | 13.3x |
| `setLocalScaling` | 75 B | 700 B | 9.3x |
| `std::string::compare` | 122 B | 1,216 B | 10.0x |
| `QueryInterface` | 20 B | 460 B | 23.0x (small absolute size) |

Extrapolated naively, 5.1 MB of `.text` becomes roughly 50 MB of native code,
which is a real instruction-cache risk on a phone. The samples are tiny, so
treat the ratio as a starting point, not a prediction. Planned work, in order
of expected payoff:

1. **Flag liveness and comparison fusion.** Flags are dead across calls and
   returns and are usually overwritten before use. Fuse `cmp`/`test` with the
   consuming `jcc`/`setcc`/`cmovcc` into direct C comparisons instead of
   materializing the six x86 flags.
2. **Register-typed function signatures.** Emit
   `uint64_t f_X(uint64_t rcx, uint64_t rdx, uint64_t r8, uint64_t r9)` so
   arguments stay in `x0`-`x3` instead of being loaded from the `CPU` struct
   on every access. Keep `CPU *` only for functions that need the full state.
3. **Identity-map the guest.** The production runtime already maps at
   `0x140000000`; dropping `MEM_BIAS` and per-access bounds checks in favor of
   guard pages plus a fault handler removes work from every load/store.
4. **XMM in NEON/vector locals.** 16% of instructions touch XMM. Preserve
   x86-specific semantics (`min`/`max` NaN, `cvtt` overflow, approximate
   `rcp`/`rsqrt`) and compile with `-ffp-contract=off`.
5. **Smaller wins.** Table-indexed indirect dispatch, `musttail` for tail
   jumps, `noreturn`/cold attributes on fatal paths, and splitting the output
   into many translation units.
6. **Build target.** Use the NDK `aarch64-linux-android` target for the app,
   not `aarch64-linux-gnu` (which links glibc). The proofs use the GNU target
   only for static codegen inspection.

Alternative lifters remain an option if the hand-written emitter becomes the
bottleneck: remill/anvill (LLVM IR) or rev.ng would supply x86-64/SSE
semantics and dead-flag elimination, at the cost of less control over the
output. Box64/FEX are excluded as execution models, but they are useful as
performance baselines for the flag-naive static output.

Design rules:

1. Guest pointers are host pointers. The guest image is mapped at its preferred
   base on 64-bit Android; if that fails, one contiguous reservation is used and
   all absolute operands are rebased at lift time. The three proofs already treat
   addresses as flat.

2. One `CPU *` per guest thread. The Windows TEB is a guest structure at a fixed
   offset; `gs:[0x30]` resolves to it. The two `gs` operands are shim points.

3. Direct calls become C calls. The callee is known from `.pdata`.

4. Indirect calls go through a dispatch table keyed by guest target address.
   Unknown targets abort with the address, never silently fall through.

5. Imported Win32 functions are thunks that marshal the Windows x64 calling
   convention into host calls. The first set is enumerated exactly in
   `analysis/binary-inventory.md`.

6. Exceptions: 876 functions have an exception handler, 2,559 have a unwind
   handler, 5,259 have chained unwind info. The cheap path is to compile with
   `-fexceptions` and translate `RaiseException`/SEH into C++ exceptions at the
   thunk boundary. The hard path is full `UNWIND_INFO` interpretation for stack
   walking. Start with the cheap path and a per-function audit.

## 4. Graphics

Imports are minimal and enumerable:

- `d3d9!Direct3DCreate9`
- 15 functions from `d3dx9_43`, including
  `D3DXCreateEffectFromFileA`, `D3DXCreateTextureFromFileInMemoryEx`,
  `D3DXCreateTexture`, `D3DXCreateCubeTextureFromFileExA`,
  `D3DXCreateTextureFromFileExA`, `D3DXSaveSurfaceToFileA`, and the
  matrix/vector helpers.

Route: D3D9 over **dxvk-native** (Vulkan), with a D3DX9 compatibility layer.
The D3DX effect VM is the single largest graphics risk: effects are compiled
bytecode, not API calls. Two options, in order of preference:

1. Parse the effect binary and translate the VM to a small interpreter that
   emits Vulkan pipeline state. This is self-contained and testable offline.
2. Pre-translate known effect files to SPIR-V at build time. Faster runtime,
   but couples the build to shipped content.

### 4.1 Native Vulkan device path (Android)

The D3D9 -> Vulkan contract is necessary but not sufficient on Android: the
platform `libvulkan.so` discovers drivers through `hw_get_module("vulkan")` and
ignores `VK_ICD_FILENAMES` on retail devices. A custom ICD therefore cannot be
registered with the system loader. The runtime instead loads the ICD directly
through the documented interface and dispatches through
`vk_icdGetInstanceProcAddr`; this is still native execution, since the ICD
submits jobs to the Mali kbase kernel driver.

Target: FristOneRR PanVK 3.7.0 (Mesa 27 base, kbase Job Manager backend), which
supports Mali-G68/Valhall v9. It exports only the three ICD entry points
(`vk_icdGetInstanceProcAddr`, `vk_icdNegotiateLoaderICDInterfaceVersion`,
`vk_icdGetPhysicalDeviceProcAddr`), so the shim negotiates interface version 5
and forwards global, instance, physical-device and device commands.

The ICD's `DT_NEEDED libhardware.so` is a VNDK-SP dependency used only for
optional gralloc/video buffers. `runtime/android/src/hardware_stub.c` provides
a freestanding `libpanvk_hw.so` that returns `-ENOENT` from `hw_get_module`,
which selects PanVK's supported no-gralloc fallback. The remaining dependencies
are public NDK libraries.

The host differential (`runtime/android/build_host.ps1`) validates negotiation,
dispatch forwarding, instance and device creation against a mock ICD. Real
device validation and the swapchain path are still open; the main integration
risk is the WSI handoff, because dxvk-native otherwise creates surfaces through
SDL2 and the platform loader. The runtime must give DXVK a surface created by
the direct loader (`ANativeWindow` -> `vkCreateAndroidSurfaceKHR`), or patch the
WSI to call it explicitly.

## 5. Audio, input, networking

- Audio: `fmodex64.dll` is loaded dynamically. Provide a host `fmodex64` shim
  that maps the FMOD Ex API onto AAudio/OpenSL. Symbols are mangled C++, so the
  shim must match the exact class layout used by the engine; treat it as a
  binary interface, not a source port.
- Input: `dinput8!DirectInput8Create` plus `xinput1_3` ordinals `2`, `3`, `5`.
  Map to Android input events. Both surfaces are small.
- Networking: 37 `ws2_32` entry points. Map to POSIX sockets. The engine is
  synchronous and blocking here; a thread-per-socket shim preserves semantics.

## 6. Risk register

| Risk | Size | Mitigation |
|---|---|---|
| Full catalog lift | 9,202 primary functions + 5,259 fragments, 1.15M instructions | Machine lift + differential per function; prioritize reachable set from entry |
| No-`.pdata` code | 5.93% of `.text`, 1,164 heuristic call targets | Symbol/prologue discovery, call-target iteration, loud unknown target |
| Vector code | 15.99% of instructions | SSE2/SSE3 subset in the lifter; ARM64 NEON lowering; per-function diff |
| Exception handling | 876 EH / 2,559 UH / 5,259 chain | C++ exceptions at thunk boundary first; unwind interpreter second |
| Indirect calls | 14,771 sites | Target table; loud failure on unknown target |
| D3DX effect VM | 15 entry points, unknown bytecode volume | Offline effect parser + SPIR-V translation |
| Native Vulkan bring-up | Direct ICD load, gralloc stub, WSI handoff | Host differential now; on-device probe; explicit `ANativeWindow` surface path |
| FMOD Ex ABI | Dynamic load, mangled C++ | Binary-interface shim, symbol-by-symbol |
| No device/emulator | - | Static + differential validation; add QEMU/device when available |
| Anti-tamper / self-checks | Unknown | Locate and neutralize only checks that block non-Windows hosts |

## 7. Effort shape

The lift is not the long pole; the shims are. A realistic split:

- Lifter and catalog: weeks. The strict-subset approach already fails loudly.
- Bulk function lift and fixups: months. Most functions are integer C.
- Exception model: months. This is the only genuinely open-ended subsystem.
- D3D9/D3DX translation: months, dominated by the effect VM.
- Audio/input/net: weeks each.
- Bring-up: unknown until a device is in the loop.

The three proofs establish that the core claim is sound: real WSE2 x86-64 code
becomes native AArch64 with identical observable behavior, including a direct
call from one lifted function into another. The remaining work is breadth, not
a missing mechanism.

## 8. Rejected approaches

- **Wine / Box64 / Winlator**: emulation or API translation, not native.
- **Dynarmic / QEMU**: dynamic recompilation, explicitly out of scope.
- **MNBTool engine mod**: the operator excluded it.
- **ARMv7 port**: Android is LP64, x64 is the cleaner ABI, and `.pdata` exists
  only in the x64 build.
