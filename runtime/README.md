# Runtime skeleton

The production-side scaffolding that lifted engine functions will execute on.
It is deliberately independent of any generated code, so it can be built and
tested without the retail executable.

## What exists

| Unit | Role |
|---|---|
| `guest.c` | reserve the guest address space (preferred `0x140000000`) |
| `pe.c` | parse and map PE32+ x86-64: headers, sections, DIR64/HIGHLOW relocs |
| `dispatch.c` | guest VA -> native function table for indirect calls |
| `imports.c` | import thunk arena and PE import binder |
| `shim.c` | Win32 shim registry, 13 KERNEL32 entry points |
| `teb.c` | TEB/PEB/TLS layout at the real Windows x64 offsets |
| `cpu.h` | CPU/flags/SSE model, identical semantics to `proof/cpu.h` |
| `selftest.c` | offline end-to-end test with a synthetic PE |

Android graphics lives in `runtime/android/`: a direct loader for the
FristOneRR PanVK ICD (Mali kbase), a `libhardware` stub, and a host-tested
probe. See `runtime/android/README.md`. It bypasses the platform
`libvulkan.so` because retail Android ignores `VK_ICD_FILENAMES`.

## Address model

Guest addresses are full VAs. The image is mapped at `0x140000000`; if that
range is unavailable, the reservation is rebased and the loader applies base
relocations. The memory layer translates once per access:

```
MEM      host pointer for guest VA MEM_BIAS
MEM_LIMIT mapped size
ld32(va) -> *(uint32_t *)(MEM + (va - MEM_BIAS))
```

Lifted code emitted for the production runtime must use absolute VAs
(image base + RVA), unlike the proof rig, which uses RVAs because it maps the
image flat at zero. `proof/lift.py` will take an `--image-base` option when the
catalog lift starts.

## Imports

Each imported function gets a unique guest VA in a reserved thunk arena, and
the guest IAT is patched with that VA. An indirect call reaches
`wse2_dispatch_call`; a missing native implementation aborts with
`DLL!Function`, never a fabricated return value. That keeps bring-up honest:
the first frame will stop at the first unimplemented import and name it.

Implemented so far: `GetTickCount64`, `GetTickCount`, `Sleep`,
`GetCurrentThreadId`, `GetCurrentProcessId`, `GetLastError`, `SetLastError`,
`GetModuleHandleA/W`, `GetProcAddress` (returns NULL for now),
`QueryPerformanceCounter`, `QueryPerformanceFrequency`, `ExitProcess`.

## TEB/PEB/TLS

Offsets are taken from the two real `gs:` accesses in the engine:

```
0x471ACC  mov rax, gs:[0x58]   ; TEB->ThreadLocalStoragePointer
0x482C1C  mov r11, gs:[0x10]   ; TEB->NtTib.StackLimit
```

`wse2_teb_build` fills TEB self (`0x30`), ClientId (`0x40`), TLS pointer
(`0x58`), PEB (`0x60`), stack base/limit, and a zeroed PEB with
`ImageBaseAddress` and Windows 10 22H2 version fields. The TLS array has 64
slots; slot 0 (the engine's `_tls_index`) points at a block initialized from
the `.tls` template.

## Build and test

```powershell
$zig = "path\to\zig.exe"
Set-Location runtime

& $zig cc -target x86_64-windows-gnu -O1 -I. -o selftest.exe `
    runtime.c guest.c pe.c dispatch.c imports.c shim.c teb.c selftest.c
.\selftest.exe
```

Expected:

```
wse2 runtime selftest: PASS
  image base      0x140000000
  sections        3, relocs 1
  imports         1 total, 1 bound, 0 missing
  teb             0x140200000  tls slot0 0x140203000
```

The self-test builds a synthetic PE32+ in memory with one KERNEL32 import and
one DIR64 relocation, maps it at the preferred base, binds the import, calls it
through dispatch, verifies the TEB/TLS access pattern and the stack-limit
probe, then maps the same image at a second base and checks that the
relocation was applied.

### Integration with the real image

With a retail executable available, generate production-mode lifts (absolute
VAs, not RVAs) and map the real PE:

```powershell
python ..\proof\lift.py <exe> 0x485390 lifted_memcmp_va.c --image-base 0x140000000
python ..\proof\lift.py <exe> 0x43E640 lifted_scaling_va.c --image-base 0x140000000

& $zig cc -target x86_64-windows-gnu -O1 -I. -o integration.exe `
    runtime.c guest.c pe.c dispatch.c integration.c `
    lifted_memcmp_va.c lifted_scaling_va.c
.\integration.exe <exe>
```

Expected:

```
wse2 runtime integration: PASS
  image           0x140000000, 6735360 bytes, 7 sections, 8875 relocs
  lifted calls    memcmp 5 cases, setLocalScaling 3 cases
  dispatch        indirect calls through guest VAs
```

This maps the actual 6.7 MB executable at `0x140000000`, applies its 8,875 base
relocations, and executes two lifted functions through `wse2_dispatch_call`
using the production memory model. `setLocalScaling` reads its RIP-relative
mask at `0x1404EFB10`, so a correct result also proves the image-base lift
option and the guest VA mapping agree.

Cross-compile every unit for the Android target:

```powershell
foreach ($f in "runtime","guest","pe","dispatch","imports","shim","teb","selftest") {
    & $zig cc -target aarch64-linux-gnu -O2 -I. -c "$f.c" -o "build_arm64/$f.o"
}
```

## Not done yet

- Thread creation and a TEB per guest thread; only the main thread layout is
  built.
- Memory ordering: `ld*`/`st*` are plain C accesses. x86-64 is strongly
  ordered; ARM64 is not. Guest `lock` operations, refcounts, and shared
  publication need C11 atomics/acquire-release before threaded engine code
  runs. Single-thread differentials do not exercise this.
- TLS callbacks and dynamic `GetProcAddress` resolution.
- Full KERNEL32/WS2_32/D3D9/D3DX9/FMOD/Lua shims.
- Exception/unwind bridge.
- Discovery of the 5.93% of `.text` that has no `.pdata` extent, so every
  direct callee can be resolved.
- Wiring the lifted catalog into dispatch and running the first guest
  instruction.
