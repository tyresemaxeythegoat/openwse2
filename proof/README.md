# Proof rig

A scoped x86-64 -> C lifter and three differential harnesses. This is not the
production lifter; it exists to validate the CPU model, flag model, memory
model, ABI, call lowering, and ARM64 codegen on real WSE2 code before the
catalog work starts.

## Files

| File | Role |
|---|---|
| `lift.py` | Capstone -> C, strict subset, fails loudly on anything unimplemented |
| `cpu.h` | guest registers, flags, memory, SSE helpers |
| `harness.c` | native runner for the `memcmp` proof |
| `harness_sse.c` | native runner for the `setLocalScaling` proof |
| `harness_call.c` | native runner for the `std::string::compare` proof |
| `diff.py` | Unicorn vs lifted C, `memcmp` |
| `diff_sse.py` | Unicorn vs lifted C, SSE + RIP-relative, compares XMM state |
| `diff_call.py` | Unicorn vs lifted C, direct call + stack frame + `cmov`/`setcc` |
| `check_arm64.py` | disassembles the ARM64 objects to prove native codegen |

No lifted C is checked in. Generate it from your own copy of the executable.

## Build

Zig 0.17.1 supplies both host x64 and AArch64 targets with no system installs.

```powershell
$zig = "path\to\zig.exe"
$exe = "path\to\mb_warband_wse2_x64.exe"

python lift.py $exe 0x485390 lifted_memcmp.c
python lift.py $exe 0x43E640 lifted_scaling.c
python lift.py $exe 0x124A0 lifted_compare.c

& $zig cc -target x86_64-windows-gnu -O1 -o harness.exe harness.c lifted_memcmp.c
& $zig cc -target x86_64-windows-gnu -O1 -o harness_sse.exe harness_sse.c lifted_scaling.c
& $zig cc -target x86_64-windows-gnu -O1 -o harness_call.exe harness_call.c lifted_compare.c lifted_memcmp.c

& $zig cc -target aarch64-linux-gnu -O2 -c lifted_memcmp.c -o lifted_memcmp_arm64.o
& $zig cc -target aarch64-linux-gnu -O2 -c lifted_scaling.c -o lifted_scaling_arm64.o
& $zig cc -target aarch64-linux-gnu -O2 -c lifted_compare.c -o lifted_compare_arm64.o

python diff.py $exe .\harness.exe .\work
python diff_sse.py $exe .\harness_sse.exe .\work_sse
python diff_call.py $exe .\harness_call.exe .\work_call
python check_arm64.py
```

Expected:

```
vectors: 495  mismatches: 0
vectors: 464  mismatches: 0
vectors: 636  mismatches: 0
```

## Model notes

- `CPU` carries `r[16]`, `flags`, `rip`, and `xmm[16]` (low 128 bits).
- Guest memory is one flat buffer. The proofs load the PE image at offset 0, so
  RVA-addressed data and RIP-relative operands resolve directly.
- `ret` pops the return address into `c->rip` and returns; the harness checks
  `rip` and `rsp` after every call.
- A direct `call` pushes the return RVA on the guest stack and becomes a C call
  to `f_<rva>`. The callee returns through the same guest stack.
- Flags cover CF, PF, AF, ZF, SF, OF. `adc`/`sbb` take an explicit carry/borrow
  input; CF is computed in 128-bit arithmetic.
- Immediates are truncated to the operation width. This matters: `sbb eax, -1`
  is `0xFFFFFFFF`, not `0xFFFFFFFFFFFFFFFF`.

## Scope

Implemented: `mov`, `movabs`, `movzx`, `movsx`, `movsxd`, `lea`, integer
add/sub/and/or/xor/cmp/test, `adc`, `sbb`, `inc`, `dec`, `neg`, `not`, shifts,
`imul` (2/3 operand), `bswap`, `setcc`, `cmovcc`, `jcc`, `jmp`, `call` (direct,
resolved at lift time), `ret`, `push`, `pop`, `leave`, `cdqe`, `cwde`, `cbw`,
`xchg`, `bt/btc/btr/bts`, `movss`, `movsd`, `movaps`, `movdqa`, `movups`,
`movdqu`, `andps`, `orps`, `xorps`, `andnps`, `pand`, `por`, `pxor`, `pandn`,
`movd`, `movq`.

Not implemented, by design: x87, `div`/`idiv`, `rep` string ops, `syscall`,
`cpuid`, `rdtsc`, exceptions, imports, indirect calls. The lifter aborts with
the address and mnemonic rather than emitting a guess.

Note on proof 3: every path of `std::string::compare` ends in the same epilogue
`add rsp, 0x20`, so the final flag word is constant across vectors. The branch
semantics are validated through the return value (`-1`, `0`, `+1` all occur),
and the flag model itself was validated by proof 1.
