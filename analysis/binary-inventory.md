# Binary inventory

Source: `mb_warband_wse2_x64.exe`, WSE2 v1.1.5.4 package
(`WSE2.zip`, 70,730,391 B,
`sha256:118ed9517356a7016bfb911bccb600a3d95682407687ec0552fdc5acdb91f06a`).

Reproduce with `tools/pe_catalog.py` and `tools/symbol_inventory.py`.

## Image

| Field | Value |
|---|---|
| Format | PE32+ |
| Machine | `0x8664` (x86-64) |
| Image base | `0x140000000` |
| Entry point | `0x140481CE0` |
| Size on disk | 6,735,360 B |
| Subsystem | Windows GUI |

## Sections

| Name | RVA | Virtual size | Raw size |
|---|---|---|---|
| `.text` | `0x00001000` | 5,125,605 | 5,125,632 |
| `.rdata` | `0x004E5000` | 898,845 | 899,072 |
| `.data` | `0x005C1000` | 1,194,817 | 56,832 |
| `.pdata` | `0x006E5000` | 173,532 | 173,568 |
| `.tls` | `0x00710000` | 9 | 512 |
| `.rsrc` | `0x00711000` | 425,656 | 425,984 |
| `.reloc` | `0x00779000` | 52,474 | 52,736 |

`.data` has a large virtual size against a small raw size: most of it is
zero-initialized globals and heap-reserved space. This matters for the guest
mapping: reserve the full virtual size.

## Function catalog

`.pdata` holds 14,464 `RUNTIME_FUNCTION` records; 3 are degenerate. Usable
extents: **14,461**, covering **4,821,894 B** of `.text` (**94.07%**).

Those extents are not 14,461 functions. 5,259 carry `UNW_FLAG_CHAININFO` and
are cold fragments of another function; the owning primary is named by the
chained `RUNTIME_FUNCTION`. The catalog therefore reports:

- **9,202 primary functions**, covering **73.52%** of `.text`
- **5,259 CHAININFO fragments**, covering another **20.55%**
- **1,700** primary functions have at least one fragment; the maximum is 22
- **5.93%** of `.text` (**303,711 B** in 8,285 ranges) has no `.pdata` entry
  at all, so it is neither a primary extent nor a fragment

`tools/pe_catalog.py` emits `kind` (`function`/`fragment`), `parent` for
fragments, and a `fragments` list on each primary. `--functions-only` writes
one row per primary function.

- 7,086 starts have a public symbol.
- 7,375 are anonymous but bounded.
- No unwind pointer falls outside `.rdata`.

### Coverage gaps

The no-`.pdata` 5.93% is not just data: leaf functions and thunks live there
and are still call targets. A conservative byte scan for `E8 rel32` finds
**1,164 unique call targets inside `.text` but outside every `.pdata` extent**
(the scan is heuristic because data bytes can look like calls). The
most-referenced targets are CRT-style thunks:

| Target | `E8` sites |
|---|---:|
| `0x47B8D4` | 6,854 |
| `0x47A0F0` | 1,862 |
| `0x28C90` | 1,795 |
| `0x3E46B4` | 1,199 |
| `0x46FA00` | 524 |

The largest contiguous gap is `0x4D60E3..0x4DD280` (29,085 B). Bulk lifting
needs a discovery pass for these ranges (public symbols, prologue patterns,
call-target iteration) before the lifter can treat every direct callee as
defined.

### Size distribution

Extent sizes, including fragments:

| Upper bound (B) | Extents |
|---|---|
| 16 | 1,128 |
| 32 | 1,491 |
| 64 | 2,452 |
| 128 | 2,620 |
| 256 | 2,939 |
| 512 | 2,087 |
| 1,024 | 981 |
| 4,096 | 649 |
| 65,536 | 114 |

### Unwind info

All records are version 1. Flags:

| Flag | Count |
|---|---|
| `EHANDLER` | 876 |
| `UHANDLER` | 2,559 |
| `CHAININFO` | 5,259 |
| none | 6,626 |

## Symbols

22,600 x64 public symbols (shipped PDB). Largest namespaces by callable
symbols:

| Class | Symbols |
|---|---|
| `std` | 241 |
| `mbAgent` | 231 |
| `Concurrency` | 223 |
| `mbGame` | 174 |
| `mbMission` | 144 |
| `mbnetNetworkManager` | 119 |
| `rglResourceManager` | 71 |
| `rglWidget` | 69 |
| `mbParty` | 68 |
| `mbTerrainGenerator` | 63 |
| `rglMesh` | 61 |
| `rglString` | 51 |
| `btDiscreteDynamicsWorld` | 45 |
| `WSELuaManager` | 44 |
| `rglScene` | 43 |

Physics is Bullet in this build: 2,300 `bt*` symbols versus 5 Havok-named
symbols. The engine's `rglRagdollWorld` sits on `btRigidBody` and
`btCollisionShape`.

## Imports

| DLL | Count | Notes |
|---|---|---|
| KERNEL32 | 223 | files, heaps, threads, TLS, sync, memory |
| lua5164 | 103 | Lua 5.1 + LuaJIT (`luaopen_jit`, `luaopen_bit`, `luaL_setfuncs`) |
| USER32 | 40 | window, message loop, cursor, clipboard, accelerators |
| WS2_32 | 37 | full blocking socket surface |
| d3dx9_43 | 15 | texture/effect creation, matrix and vector math |
| WINHTTP | 11 | HTTP client |
| ADVAPI32 | 10 | registry + CryptoAPI hashing |
| dbghelp | 8 | symbol handling, `StackWalk64`, minidumps |
| ole32 | 5 | COM init / `CoCreateInstance` |
| OLEAUT32 | 5 | BSTR / VARIANT |
| IMM32 | 4 | IME |
| SHLWAPI | 4 | URL + path helpers |
| WINMM | 3 | `timeGetTime`, timer resolution |
| XINPUT1_3 | 3 | ordinals `2`, `3`, `5` |
| SHELL32 | 2 | `ShellExecuteA`, `SHGetFolderPathA` |
| PSAPI | 1 | `GetProcessMemoryInfo` |
| GDI32 | 1 | `GetStockObject` |
| urlmon | 1 | `URLDownloadToFileA` |
| WININET | 1 | `DeleteUrlCacheEntry` |
| **d3d9** | **1** | `Direct3DCreate9` |
| **DINPUT8** | **1** | `DirectInput8Create` |

### Graphics, exact

```
d3d9.dll    Direct3DCreate9

d3dx9_43.dll
  D3DXMatrixPerspectiveOffCenterRH
  D3DXMatrixOrthoOffCenterRH
  D3DXMatrixTranspose
  D3DXMatrixInverse
  D3DXMatrixMultiply
  D3DXPlaneNormalize
  D3DXVec3Normalize
  D3DXVec4Normalize
  D3DXVec4Transform
  D3DXCreateTexture
  D3DXCreateTextureFromFileInMemoryEx
  D3DXCreateTextureFromFileExA
  D3DXCreateCubeTextureFromFileExA
  D3DXCreateEffectFromFileA
  D3DXSaveSurfaceToFileA
```

### Dynamically loaded

Strings present in the image:

```
fmodex.dll  fmodex64.dll  fmodex_wse2.dll
steam_api.dll  steam_api64.dll  steam_api_wse2.dll
lua5164.dll
d3d9.dll  d3dx9_43.dll  dinput8.dll  xinput1_3.dll
```

`LoadLibraryA`/`LoadLibraryExA`/`LoadLibraryW` are all imported, and
`GetProcAddress` is imported, so the runtime must serve dynamic lookups for
these names, not just static imports.

## Instruction mix

Per-extent scan, 1,151,840 instructions:

| Feature | Count | Share |
|---|---|---|
| Vector register operands | 184,227 | 15.99% |
| `jcc` | 107,410 | 9.33% |
| Direct call | 68,618 | 5.96% |
| RIP-relative memory | 67,136 | 5.83% |
| Direct jump | 25,337 | 2.20% |
| Indirect call | 14,771 | 1.28% |
| `cmov` | 3,975 | 0.35% |
| `setcc` | 2,161 | 0.19% |
| Trap | 1,201 | 0.10% |
| Indirect jump | 419 | 0.04% |
| `div`/`idiv` | 329 | 0.03% |
| Locked | 327 | 0.03% |
| High-8 register | 239 | 0.02% |
| `rep` | 157 | 0.01% |
| `cpuid`/`rdtsc` | 11 | ~0 |
| x87 register | 10 | ~0 |
| `gs` segment | 2 | ~0 |

## Shim surface, first cut

Anything below is a hard requirement before first frame:

- **Kernel32**: file I/O (`CreateFileA/W`, `ReadFile`, `WriteFile`, mapping),
  heap (`HeapAlloc/Free/ReAlloc`, `GetProcessHeap`), sync
  (`EnterCriticalSection`, `WaitForSingleObject`, events, semaphores),
  threads (`CreateThread`, TLS), time (`QueryPerformanceCounter`,
  `GetTickCount64`), and `LoadLibrary*`/`GetProcAddress`.
- **NTDLL**: `RtlVirtualUnwind`, `RtlLookupFunctionEntry`, `RtlUnwindEx`,
  `RtlCaptureContext` are imported from kernel32's forwarding layer and must be
  answered by the unwind shim.
- **D3D9/D3DX9**: one device creation entry, 15 helpers.
- **DInput8/XInput**: device enumeration and state polling.
- **WinSock**: 37 entry points over POSIX.
- **Lua**: 103 exports; ship a real LuaJIT 2.0/5.1-compatible library, not a
  stub. The engine relies on `luaopen_jit`.
- **FMOD Ex**: dynamic; shim by mangled symbol.
- **Steam**: dynamic; the engine tolerates absence, but `steam_api64.dll`
  lookups must resolve to a stub with `SteamAPI_Init` returning false.
