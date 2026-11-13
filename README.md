# wse2-android

A clean-room, evidence-backed **C++20 reconstruction of the WSE2** (Warband Script Enhancer 2)
engine, aimed at running natively on **Android arm64-v8a**. No emulator, no Wine/Box64/Dynarmic,
no MNBTool mod.

The project is **bring-your-own-assets**: it contains no game files, no retail executables or
PDBs, and no lifted machine code. The eventual Android app loads a Warband installation and
`Modules/` folder that the user already owns. It is not affiliated with TaleWorlds Entertainment.

## Where things stand

The project is early: the module-system script VM is being reconstructed and tested on a desktop
host. There is no renderer, audio, input, or Android app in the clean-room engine yet, and nothing
has been compiled for arm64. Live numbers and gate results are in [`STATUS.md`](STATUS.md).

| Read this | To learn |
| --- | --- |
| [`STATUS.md`](STATUS.md) | What passes, what is covered, what is blocked, what is next |
| [`ROADMAP.md`](ROADMAP.md) | Phases, ordered engineering backlog, milestone log |
| [`AGENTS.md`](AGENTS.md) | Rules for contributors and coding agents: evidence, gates, doc upkeep |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | Recovered layouts, addresses, and evidence records |
| [`engine/README.md`](engine/README.md) | What each portable module implements and how to test it |

## Repository layout

| Path | Status | Contents |
| --- | --- | --- |
| `engine/` | active | Portable C++20 headers (`include/wse/{io,vm,game}`), host tests, fixtures |
| `docs/` | active | Architecture notes, symbol map, generated WSE opcode map |
| `tools/` | mixed | Evidence extractors and generators (active); `pe_catalog.py` and the PanVK helpers are legacy |
| `analysis/`, `proof/`, `runtime/` | legacy | The superseded static-recompilation approach, kept as CPU/ABI reference |

## Build and test

Windows (Zig's `clang++`):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1
```

Linux: see the host-gate loop in [`AGENTS.md`](AGENTS.md) §2. Both must end with
`engine host tests: PASS`.

## License

Tooling and analysis in this repository: MIT (see [`LICENSE`](LICENSE)). Engine symbols and any
generated code remain the property of their respective rights holders.

---

# Historical: static recompilation (superseded 2026-11)

Everything below describes the project's first approach — lifting the x64 machine code to C and
recompiling it. It was replaced by the clean-room reconstruction above and is kept unchanged as
CPU/ABI reference. Its figures refer to the older 6,735,360-byte build, not the v1.1.5.5 baseline.

Static recompilation of the **WSE2** (Warband Script Enhancer 2) x64 engine to
native **Android ARM64**. No MNBTool mod, no emulator, no Wine/Box64/Dynarmic.

This repository holds the toolchain, analysis, and validation rigs. It does not
contain game files, it does not contain lifted engine code derived from
them, and this project is not affiliated with TaleWorlds Entertainment. Lifted code is generated locally from a copy of the retail executable.

## What is proven

Four real engine functions have been lifted from x86-64 to portable C and
executed natively, with differential validation against the original machine
code under Unicorn:

| Function | RVA | Size | Instructions | Vectors | Mismatches |
|---|---|---|---|---|---|
| CRT `memcmp` | `0x485390` | 199 B | 69 | 495 | 0 |
| `btConvexInternalShape::setLocalScaling` | `0x43E640` | 75 B | 17 | 464 | 0 |
| `std::string::compare` (calls `memcmp`) | `0x124A0` | 122 B | 39 | 636 | 0 |
| `rglEffectStateManager::QueryInterface` (vtable call) | `0x281B50` | 20 B | 7 | 464 | 0 |

All four reach the same result on the x86-64 host build and cross-compile to
real AArch64 machine code (verified by disassembling the ARM64 objects: 660,
175, 304, and 115 AArch64 instructions, `stp/ldp/eor/csel/b.lo`, no
interpreter).

The operand-width and flag model is also validated against the real x86-64
host CPU, not only Unicorn: `flagdiff.py` synthesizes instruction streams,
runs them through a Win64 trampoline, and compares registers, flags, and XMM
state against the lifted C. The final run passes **14,200 vectors with 0
mismatches** across 8/16/32/64-bit shifts, 16/32-bit `imul`, 32-bit `cmovcc`,
and `movd`/`movq` GPR/XMM/memory forms. Before the fixes, the same rig
reported **6,079 mismatches in 14,000 vectors**.

The proofs deliberately cover the hardest "easy" features:

- RIP-relative data access resolved at lift time (`0x4EFB10`, the `0x7FFFFFFF`
  abs mask in `.rdata`)
- SSE state: `movss`, `movaps`, `movdqa`, `andps`, `xorps`, including the
  zero-upper-bits rule for `movss` loads
- direct calls: guest-stack return address, full MSVC frame with spills,
  `cmovb` / `setne` on the flag model
- a lifted function calling another lifted function with the real
  `std::string` layout (SSO and heap buffers)
- indirect calls: a vtable slot loaded from guest memory, target evaluated
  before the return-address push, resolved through `wse2_dispatch_call`
- GS-relative (TEB) addressing lowered through `c->teb`; FS and other segment
  overrides abort the lift instead of being silently dropped

## Binary under analysis

`mb_warband_wse2_x64.exe`

- PE32+ x86-64, image base `0x140000000`, entry `0x140481CE0`
- 6,735,360 bytes
- `.text`: 5,125,605 bytes
- `.pdata`: 14,461 usable extents = 9,202 primary functions + 5,259
  `CHAININFO` cold fragments
- extent coverage **94.07%** of `.text`; primary coverage **73.52%**
- the remaining **5.93%** (303,711 B) is leaf code/thunks with no `.pdata`
  record, including 1,164 heuristic direct-call targets
- 22,600 public symbols available from the shipped PDB
- 1,151,840 instructions across the `.pdata` extents

## Pipeline

```
mb_warband_wse2_x64.exe
        |
        |  tools/pe_catalog.py        (.pdata extents + PDB publics)
        v
  function catalog (RVA, size, name, kind, parent/fragments, unwind flags)
        |
        |  proof/lift.py               (Capstone -> C, strict subset)
        v
  f_<rva>(CPU *c)  generated C
        |
        |  zig cc -target aarch64-linux-gnu
        v
  native AArch64 objects -> Android NativeActivity shell
```

Runtime shape follows the proven Android static-recomp pattern: fixed guest
address space, explicit CPU state, direct calls lowered to C calls, import
thunks, and indirect dispatch through a resolved target table.

The runtime skeleton lives in `runtime/`: guest reservation, PE32+ mapping with
relocations, import thunk arena, indirect dispatch, TEB/PEB/TLS at the real x64
offsets, and a small Win32 shim registry. `runtime/selftest.exe` validates it
offline against a synthetic PE (one import, one relocation, TLS slot, stack
limit), and every unit cross-compiles for AArch64.

`runtime/integration.exe` exercises the real image: it maps the retail 6.7 MB
executable at its preferred base `0x140000000`, applies all 8,875 base
relocations, and calls two production-mode lifts through `wse2_dispatch_call`.
`setLocalScaling` reads its RIP-relative mask at `0x1404EFB10`, so a passing run
also proves the image-base lift option and the guest VA mapping agree.

## What is not done

- Wiring the full lifted catalog into the runtime; two functions execute today,
  the other 9,200 primary functions are not lifted yet.
- Full catalog lift: 9,202 primary functions plus 5,259 fragments, 1.15M
  instructions. The 5.93% of `.text` without `.pdata` still needs a discovery
  pass before every callee can be resolved.
- Exception model: 876 `EHANDLER`, 2,559 `UHANDLER`, 5,259 `CHAININFO`
  fragments.
- Graphics: D3D9 + 15 `d3dx9_43` entry points over Vulkan/dxvk-native.
- Audio: FMOD Ex loaded dynamically (`fmodex64.dll`).
- Input: `dinput8` + `xinput1_3`.
- No device or emulator is available in this workspace, so validation is
  static and differential only.
- Memory ordering: lifted code uses plain C loads/stores and assumes x86
  ordering. ARM64 needs atomics/acquire-release at shared handoff points; this
  is not implemented yet and single-thread differentials cannot catch it.
- Code-size optimization: the correctness-first emitter expands x86 text
  roughly 10x in the proof samples. Flag fusion, register-typed signatures,
  identity mapping, and NEON locals are planned before the bulk lift.

## Reproduce the proofs

Requires Python 3 with the versions pinned in `proof/requirements.txt`
(`capstone==5.0.7`, `pefile==2024.8.26`, `unicorn==2.1.4`) and Zig 0.17.1.

```powershell
# host harnesses
zig cc -target x86_64-windows-gnu -O1 -o harness.exe harness.c lifted_memcmp.c
zig cc -target x86_64-windows-gnu -O1 -o harness_sse.exe harness_sse.c lifted_scaling.c
zig cc -target x86_64-windows-gnu -O1 -o harness_call.exe harness_call.c lifted_compare.c lifted_memcmp.c
zig cc -target x86_64-windows-gnu -O1 -o harness_indirect.exe harness_indirect.c lifted_queryinterface.c lifted_scaling.c

# lift four functions from your own copy
python lift.py <path-to>mb_warband_wse2_x64.exe 0x485390 lifted_memcmp.c
python lift.py <path-to>mb_warband_wse2_x64.exe 0x43E640 lifted_scaling.c
python lift.py <path-to>mb_warband_wse2_x64.exe 0x124A0 lifted_compare.c
python lift.py <path-to>mb_warband_wse2_x64.exe 0x281B50 lifted_queryinterface.c

# differential vs the original machine code
python diff.py <exe> ./harness.exe ./work
python diff_sse.py <exe> ./harness_sse.exe ./work_sse
python diff_call.py <exe> ./harness_call.exe ./work_call
python diff_indirect.py <exe> ./harness_indirect.exe ./work_indirect

# operand-width/flag differential vs the real host CPU (Windows x64)
python flagdiff.py --zig <zig.exe>

# GS/TLS addressing and segment-override rejection
python seg_test.py --zig <zig.exe>

# runtime integration: production addressing, real image
python lift.py <exe> 0x485390 lifted_memcmp_va.c --image-base 0x140000000
python lift.py <exe> 0x43E640 lifted_scaling_va.c --image-base 0x140000000
zig cc -target x86_64-windows-gnu -O1 -I ..\runtime -o integration.exe `
    ..\runtime\runtime.c ..\runtime\guest.c ..\runtime\pe.c ..\runtime\dispatch.c `
    ..\runtime\integration.c lifted_memcmp_va.c lifted_scaling_va.c
./integration.exe <exe>
```

## References

Prior art studied for pipeline shape, not copied:

- `codepdbh/nfsmwpc-recompiled`
- `ttom795/Skyrim_Android`
- `Gabrick75/fh2-recompiled`
- `kvse/dsr-native`
- `mamcjr/BOTWRecompAndroid`
- `caos-obliquo/tboi-recomp`
