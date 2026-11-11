# Proof results

Two real WSE2 x64 functions were lifted to C and validated against the original
machine code in Unicorn. Both were then cross-compiled to AArch64.

Environment: Windows x64 host, Python 3.12, `pefile`, `capstone` 5.0.7,
`unicorn` 2.1.4, Zig 0.17.1.

## Proof 1: CRT `memcmp`

- RVA `0x485390`, extent `0x485390..0x485457`
- 199 bytes, 69 instructions, 3 returns
- No calls, no vector code, no exceptions
- Byte loop, qword loop, `bswap`, `sbb` result normalization

Lift:

```
python lift.py <exe> 0x485390 lifted_memcmp.c
lifted 0x485390..0x485457 (199 bytes, 69 instructions)
```

Differential: 495 vectors (lengths 0..80, equal, single-bit flips, signed
boundary pairs, all-zero vs all-ones, random).

```
vectors: 495  mismatches: 0
```

### Bug found by the differential

The first run produced 213 mismatches. The lift computed `sbb eax, eax` /
`sbb eax, -1` as returning `0` where hardware returns `-1`.

Two defects, both in the flag model:

1. `flags_sub` ignored borrow-in when computing CF. `0xFFFFFFFF - 0xFFFFFFFF - 1`
   must set CF, because the carry-in forces a borrow.
2. Immediate operands were not truncated to the operation width. `sbb eax, -1`
   used `0xFFFFFFFFFFFFFFFF` as the subtrahend instead of `0xFFFFFFFF`.

Fix: `flags_add`/`flags_sub` take an explicit carry/borrow input and compute CF
in 128-bit arithmetic; `read_op` masks immediates to the operation size. After
the fix: zero mismatches.

This is exactly the failure mode the differential rig exists for. A static
review would not have caught it.

## Proof 2: `btConvexInternalShape::setLocalScaling`

Symbol: `?setLocalScaling@btConvexInternalShape@@UEAAXAEBVbtVector3@@@Z`
- RVA `0x43E640`, extent `0x43E640..0x43E68B`
- 75 bytes, 17 instructions
- Covers `movss`, `movaps`, `movdqa`, `andps`, `xorps`
- Covers RIP-relative data: `movss xmm2, [rip + 0xB14C0]`
  resolves to `0x4EFB10` = `0x7FFFFFFF` x4 (the abs mask)

Lift:

```
python lift.py <exe> 0x43E640 lifted_scaling.c
lifted 0x43E640..0x43E68B (75 bytes, 17 instructions)
```

Differential: 464 vectors, comparing the 16 bytes written at `[rcx+0x20]`
**and all 16 XMM registers**. Vectors include `±0`, `±inf`, two NaN payloads,
max/min normals, denormals, sign-only patterns, and 400 random triples.

```
vectors: 464  mismatches: 0
```

The XMM comparison is what makes this meaningful: `movss` loads must zero bits
32..127, and `xorps xmm0, xmm0` must clear all 128 bits. The harness seeds every
XMM register with a distinct byte pattern (`0xA0+i`) before the call, so any
failure to clear or preserve is visible.

## ARM64 code generation

Both generated C files cross-compile for `aarch64-linux-gnu` and disassemble to
ordinary AArch64:

```
lifted_memcmp_arm64.o : .text 2640 B, 660 AArch64 instructions
lifted_scaling_arm64.o: .text  700 B, 175 AArch64 instructions
```

Representative instructions:

```
stp  x29, x30, [sp, #-0x20]!
ldp  x10, x9, [x0, #8]
sub  x11, x9, x10
cmp  x12, #8
b.lo #0x568
eor  w12, w12, w12, lsr #1
csel w9, w9, wzr, eq
```

No interpreter, no bytecode, no dispatch loop around the lifted body. This is
the engine's logic compiled into native ARM64.

## What this establishes

- The guest CPU/flags model matches hardware on integer and SSE operations.
- RIP-relative accesses can be resolved at lift time to flat guest addresses.
- The generated C shape (`uint32_t f_<rva>(CPU *c)`) is ABI-neutral and
  compiles for both host x64 and target AArch64.
- Differential testing catches real semantic bugs at function granularity.

## What it does not establish

- Exception/unwind behavior. Neither proof throws.
- Vector code beyond the SSE subset above.
- Indirect calls, imports, TLS, or any OS interaction.
- Anything about D3D9, D3DX, FMOD, or Lua.

Next proofs should target: one function with an indirect call, one with a
`__try`/`__except` block (`EHANDLER`), and one heavy SSE math routine.
