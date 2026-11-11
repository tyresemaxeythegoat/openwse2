# wse2-android

Static recompilation of the **WSE2** (Warband Script Enhancer 2) x64 engine to
native **Android ARM64**. No MNBTool mod, no emulator, no Wine/Box64/Dynarmic.

This repository holds the toolchain, analysis, and validation rigs. It does not
contain game files, and it does not contain lifted engine code derived from
them. Lifted code is generated locally from a copy of the retail executable.

## What is proven

Three real engine functions have been lifted from x86-64 to portable C and
executed natively, with differential validation against the original machine
code under Unicorn:

| Function | RVA | Size | Instructions | Vectors | Mismatches |
|---|---|---|---|---|---|
| CRT `memcmp` | `0x485390` | 199 B | 69 | 495 | 0 |
| `btConvexInternalShape::setLocalScaling` | `0x43E640` | 75 B | 17 | 464 | 0 |
| `std::string::compare` (calls `memcmp`) | `0x124A0` | 122 B | 39 | 636 | 0 |

All three reach the same result on the x86-64 host build and cross-compile to
real AArch64 machine code (verified by disassembling the ARM64 objects: 660,
175, and 304 AArch64 instructions, `stp/ldp/eor/csel/b.lo`, no interpreter).

The proofs deliberately cover the hardest "easy" features:

- RIP-relative data access resolved at lift time (`0x4EFB10`, the `0x7FFFFFFF`
  abs mask in `.rdata`)
- SSE state: `movss`, `movaps`, `movdqa`, `andps`, `xorps`, including the
  zero-upper-bits rule for `movss` loads
- direct calls: guest-stack return address, full MSVC frame with spills,
  `cmovb` / `setne` on the flag model
- a lifted function calling another lifted function with the real
  `std::string` layout (SSO and heap buffers)

## Binary under analysis

`mb_warband_wse2_x64.exe`

- PE32+ x86-64, image base `0x140000000`, entry `0x140481CE0`
- 6,735,360 bytes
- `.text`: 5,125,605 bytes
- `.pdata`: 14,461 usable function extents covering **94.07%** of `.text`
- 22,600 public symbols available from the shipped PDB
- 1,151,840 instructions across the `.pdata` extents

## Pipeline

```
mb_warband_wse2_x64.exe
        |
        |  tools/pe_catalog.py        (.pdata extents + PDB publics)
        v
  function catalog (RVA, size, name, unwind flags)
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

## What is not done

- Full catalog lift: 14,461 functions, 1.15M instructions.
- Exception model: 876 `EHANDLER`, 2,559 `UHANDLER`, 5,259 `CHAININFO`.
- Graphics: D3D9 + 15 `d3dx9_43` entry points over Vulkan/dxvk-native.
- Audio: FMOD Ex loaded dynamically (`fmodex64.dll`).
- Input: `dinput8` + `xinput1_3`.
- No device or emulator is available in this workspace, so validation is
  static and differential only.

## Reproduce the proofs

Requires Python 3 with `pefile`, `capstone`, `unicorn`, plus Zig 0.17.1.

```powershell
# host harnesses
zig cc -target x86_64-windows-gnu -O1 -o harness.exe harness.c lifted_memcmp.c
zig cc -target x86_64-windows-gnu -O1 -o harness_sse.exe harness_sse.c lifted_scaling.c
zig cc -target x86_64-windows-gnu -O1 -o harness_call.exe harness_call.c lifted_compare.c lifted_memcmp.c

# lift three functions from your own copy
python lift.py <path-to>mb_warband_wse2_x64.exe 0x485390 lifted_memcmp.c
python lift.py <path-to>mb_warband_wse2_x64.exe 0x43E640 lifted_scaling.c
python lift.py <path-to>mb_warband_wse2_x64.exe 0x124A0 lifted_compare.c

# differential vs the original machine code
python diff.py <exe> ./harness.exe ./work
python diff_sse.py <exe> ./harness_sse.exe ./work_sse
python diff_call.py <exe> ./harness_call.exe ./work_call
```

## References

Prior art studied for pipeline shape, not copied:

- `codepdbh/nfsmwpc-recompiled`
- `ttom795/Skyrim_Android`
- `Gabrick75/fh2-recompiled`
- `kvse/dsr-native`
- `mamcjr/BOTWRecompAndroid`
- `caos-obliquo/tboi-recomp`

## License

Tooling and analysis in this repository: MIT. Engine symbols and any generated
code remain the property of their respective rights holders.
