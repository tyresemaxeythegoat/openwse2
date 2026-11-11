# Proof results

Four real WSE2 x64 functions were lifted to C and validated against the
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

## Operand-width and flag differential vs real hardware

Unicorn is a good model, but it is still a model. `flagdiff.py` therefore
synthesizes `instruction; ret` streams, lifts them with `proof/lift.py`, and
runs the original bytes on the host x86-64 CPU through `hw_x86.S`. The
trampoline preserves the Win64 callee-saved set (`RBX`, `RBP`, `RDI`, `RSI`,
`R12`-`R15`, `XMM6`-`XMM15`) and executes each stream with a real host stack.
The driver compares all 16 GPRs, the architecturally defined flags, and the
full 128-bit state of all 16 XMM registers.

Coverage: 20 instruction cases and 14,200 vectors, including 8/16/32/64-bit
`shl`/`shr`/`sar`, `sar eax, 31`, 2-operand `imul16`/`imul32`, 32-bit
`cmovne`/`cmovae`, and `movd`/`movq` between GPRs, XMM registers, and memory.

Initial run: **6,079 mismatches in 14,000 vectors**. After the fixes:
**0 mismatches in 14,200 vectors**.

| Case | Before | Case | Before |
|---|---:|---|---:|
| `sar8` | 438/600 | `sar16` | 390/600 |
| `sar32` | 342/800 | `sar32_31` | 46/200 |
| `shl8` | 508/800 | `shl16` | 365/800 |
| `shl32` | 16/800 | `shr8` | 530/800 |
| `shr16` | 341/800 | `shr32` | 16/800 |
| `imul16` | 968/1200 | `imul32` | 1317/1600 |
| `cmovne32` | 217/800 | `cmovae32` | 201/800 |
| `movd xmm,eax` | 188/200 | `movq xmm,rax` | 196/200 |

Bugs the differential caught and the fixes:

- `sar r8/r16/r32` lost sign fill because the operand was zero-extended
  before the arithmetic shift. Fixed with `sext_bits` / `sar_bits`.
- 8/16/32-bit shift counts were masked with `bits - 1` instead of `31`, so
  `shl al, 9` shifted by 1. Fixed.
- A 32-bit shift by zero must still write EAX to clear bits 32-63. The old
  code skipped the write when the masked count was zero. Fixed.
- `flags_shift` shifted by a negative amount when the count exceeded the
  operand width. CF/OF are now only computed when defined.
- `flags_imul` ignored operand width; `imul16`/`imul32` flags were computed as
  if the operands were 64-bit. It now sign-extends at the operation width and
  compares against the sign-extended truncated result. Only CF/OF are tested;
  AF/PF/ZF/SF are architecturally undefined and ignored.
- A 32-bit `cmovcc` zero-extends the destination even when the condition is
  false. The old false path left bits 32-63 untouched. Fixed.
- `movd`/`movq` treated a GPR source as memory (`ld32(0)`), and
  `movq xmm, xmm` reached the GPR writer and failed. Both are now explicit,
  including memory stores and the zero-upper-128 rule.
- `lift()` caught every non-`SystemExit` exception, emitted an `UNSUPPORTED`
  comment, and exited 0. A missing opcode is now a fatal lift error.
- `gs:` overrides are lowered through `CPU::teb`; `fs:` and every other
  segment override abort with the instruction address. `seg_test.py` checks
  both `gs:[disp32]` and `gs:[reg]`, and verifies the FS rejection.

## Proof 4: `rglEffectStateManager::QueryInterface` indirect call

Symbol: `?QueryInterface@rglEffectStateManager@@UEAAJAEBU_GUID@@PEAPEAX@Z`
- RVA `0x281B50`, extent `0x281B50..0x281B64`
- 20 bytes, 7 instructions
- Loads the object's vtable from `[rcx]`, then `call qword ptr [rax+8]`
- Stores `this` into `*ppv` and returns S_OK
- The real vtable slot 1 is `AddRef` (`0x281B70`), which is outside `.pdata`

Lift:

```
python lift.py <exe> 0x281B50 lifted_queryinterface.c
lifted 0x281B50..0x281B64 (20 bytes, 7 instructions)
```

The indirect call is lowered as:

```c
{ uint64_t t_ = ld64((c->r[0] + 0x8ull));
  c->r[RSP] -= 8; st64(c->r[RSP], 0x281B5Dull);
  wse2_dispatch_call(c, t_); }
```

The target is evaluated before the stack push, matching x86 semantics for
operands that read `rsp`. The return address is still a guest stack value, and
the dispatcher maps the loaded target to a lifted C function.

Because the real slot 1 (`AddRef`) has no `.pdata` extent, the proof points the
fake vtable at another lifted function, `setLocalScaling` (`0x43E640`). That
chains two lifted frames through an indirect call and gives the test an
observable memory effect: the callee rewrites `[this+0x20]` with the absolute
value of the vector passed in `rdx`.

Differential: 464 vectors, same edge-case pool as proof 2. Unicorn maps the
image at `0x140000000` and the vtable slot holds `0x14043E640`; the lifted
harness maps the image flat at RVA 0 and the slot holds `0x43E640`. Compared:
return value, arithmetic flags, `*ppv == this`, the vtable pointer, all four
words written by the callee, and exactly one dispatch to the expected target.

```
vectors: 464  mismatches: 0
```

## ARM64 code generation

All four generated C files cross-compile for `aarch64-linux-gnu` and
disassemble to ordinary AArch64:

```
lifted_memcmp_arm64.o        : .text 2640 B, 660 AArch64 instructions
lifted_scaling_arm64.o       : .text  700 B, 175 AArch64 instructions
lifted_compare_arm64.o       : .text 1216 B, 304 AArch64 instructions
lifted_queryinterface_arm64.o: .text  460 B, 115 AArch64 instructions
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

- The guest CPU/flags model matches the real host CPU on 14,200 width/edge
  vectors as well as on the four function-level Unicorn differentials.
- RIP-relative accesses can be resolved at lift time to flat guest addresses.
- Direct calls lower to C calls with a faithful guest stack frame, including
  callee-saved register spill slots and the return-address protocol.
- Indirect calls resolve through a guest-VA dispatch table; the target is read
  before the return-address push, and an unregistered target fails loudly.
- `cmovcc` and `setcc` lower correctly on top of the flag model.
- `gs:`-relative TEB access lowers through `c->teb`; unsupported segment
  overrides fail the lift instead of silently producing wrong addresses.
- The generated C shape (`uint32_t f_<rva>(CPU *c)`) is ABI-neutral and
  compiles for both host x64 and target AArch64.
- Differential testing catches real semantic bugs at function granularity.

## What it does not establish

- Exception/unwind behavior. None of the proofs throw.
- Vector code beyond the SSE subset above.
- Threaded TLS lifetime and per-thread TEB switching; the GS test uses one
  synthetic TEB pointer.
- Imports or any OS interaction beyond the shim-thunk integration test.
- Anything about D3D9, D3DX, FMOD, or Lua.

Next proofs should target: one function with a `__try`/`__except` block
(`EHANDLER`) and one heavy SSE math routine.
