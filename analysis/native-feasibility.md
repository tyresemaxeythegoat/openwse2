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
| Code coverage by `.pdata` | 94.07% |
| Public symbols | 22,600 |
| Instructions in extents | 1,151,840 |

`.pdata` gives exact start and end for every function. That removes the two
classic blockers of static lifting: function boundary discovery and jump-table
membership. The shipped PDB supplies names for 7,086 of the 14,461 starts;
the rest are anonymous but bounded.

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
- Locked operations are rare, which keeps the memory model simple: a guest lock
  can be a host mutex/atomic pair without a full memory-ordering engine.

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

Design rules:

1. Guest pointers are host pointers. The guest image is mapped at its preferred
   base on 64-bit Android; if that fails, one contiguous reservation is used and
   all absolute operands are rebased at lift time. The two proofs already treat
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
| Full catalog lift | 14,461 functions, 1.15M instructions | Machine lift + differential per function; prioritize reachable set from entry |
| Vector code | 15.99% of instructions | SSE2/SSE3 subset in the lifter; ARM64 NEON lowering; per-function diff |
| Exception handling | 876 EH / 2,559 UH / 5,259 chain | C++ exceptions at thunk boundary first; unwind interpreter second |
| Indirect calls | 14,771 sites | Target table; loud failure on unknown target |
| D3DX effect VM | 15 entry points, unknown bytecode volume | Offline effect parser + SPIR-V translation |
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

The two proofs establish that the core claim is sound: real WSE2 x86-64 code
becomes native AArch64 with identical observable behavior. The remaining work
is breadth, not a missing mechanism.

## 8. Rejected approaches

- **Wine / Box64 / Winlator**: emulation or API translation, not native.
- **Dynarmic / QEMU**: dynamic recompilation, explicitly out of scope.
- **MNBTool engine mod**: the operator excluded it.
- **ARMv7 port**: Android is LP64, x64 is the cleaner ABI, and `.pdata` exists
  only in the x64 build.
