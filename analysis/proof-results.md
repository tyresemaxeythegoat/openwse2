# Proof results

Three real WSE2 x64 functions were lifted to C and validated against the
original machine code in Unicorn. All were then cross-compiled to AArch64.

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

## Proof 3: `std::string::compare` calling `memcmp`

Symbol: `?compare@?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@QEBAHAEBV1@@Z`
- RVA `0x124A0`, extent `0x124A0..0x1251A`
- 122 bytes, 39 instructions
- Direct `call 0x485390` - the proof 1 function
- Full MSVC stack frame: `mov [rsp+8],rbx`, `mov [rsp+0x10],rsi`,
  `push rdi`, `sub rsp,0x20`, and the matching epilogue
- `cmovb`, `setne`, `cdqe`, `jmp`, two returns

Lift:

```
python lift.py <exe> 0x124A0 lifted_compare.c
lifted 0x124A0..0x1251A (122 bytes, 39 instructions)
```

The lifter lowers the direct call as:

```c
{ c->r[RSP] -= 8; st64(c->r[RSP], 0x124E5ull); f_485390(c); }
```

That is, the return address is pushed on the *guest* stack and the callee is a
plain C call. `memcmp` returns by popping the guest return address into
`c->rip`; control then continues at `L_124E5`.

Differential: 636 vectors. Both `std::string` objects are built with the real
x64 layout (16-byte inline buffer, size at `+0x10`, capacity at `+0x18`),
covering SSO and heap strings, equal prefixes, divergent bytes at every
position, differing lengths, and 200 random pairs. Compared: return value and
the arithmetic flags.

```
vectors: 636  mismatches: 0
```

Return value distribution across the vectors: `-1` x260, `0` x69, `+1` x307.
All three exit paths are exercised.

One honest caveat: every path ends in the same epilogue `add rsp, 0x20`, so the
final flag word is constant across all 636 vectors. The `cmovb`/`setne` logic
is validated through the return value, and integer flag semantics were already
validated by proof 1. The harness sets the guest stack to the same low 32 bits
as the Unicorn stack so the epilogue's flag effects are identical.

## ARM64 code generation

All three generated C files cross-compile for `aarch64-linux-gnu` and
disassemble to ordinary AArch64:

```
lifted_memcmp_arm64.o : .text 2640 B, 660 AArch64 instructions
lifted_scaling_arm64.o: .text  700 B, 175 AArch64 instructions
lifted_compare_arm64.o: .text 1216 B, 304 AArch64 instructions
```

Representative instructions:

```
stp  x29, x30, [sp, #-0x30]!
str  x21, [sp, #0x10]
ldp  x10, x9, [x0, #8]
sub  x11, x9, x10
cmp  x12, #8
b.lo #0x568
b.hi #0x468
eor  w12, w12, w12, lsr #1
csel w9, w9, wzr, eq
```

No interpreter, no bytecode, no dispatch loop around the lifted body. This is
the engine's logic compiled into native ARM64.

## What this establishes

- The guest CPU/flags model matches hardware on integer and SSE operations.
- RIP-relative accesses can be resolved at lift time to flat guest addresses.
- Direct calls lower to C calls with a faithful guest stack frame, including
  callee-saved register spill slots and the return-address protocol.
- `cmovcc` and `setcc` lower correctly on top of the flag model.
- The generated C shape (`uint32_t f_<rva>(CPU *c)`) is ABI-neutral and
  compiles for both host x64 and target AArch64.
- Differential testing catches real semantic bugs at function granularity.

## What it does not establish

- Exception/unwind behavior. None of the proofs throw.
- Vector code beyond the SSE subset above.
- Indirect calls, imports, TLS, or any OS interaction.
- Anything about D3D9, D3DX, FMOD, or Lua.

Next proofs should target: one function with an indirect call, one with a
`__try`/`__except` block (`EHANDLER`), and one heavy SSE math routine.
