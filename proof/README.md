# Proof rig

A scoped x86-64 -> C lifter, four function differential harnesses, and a
hardware differential for the CPU/flag model. This is not the production
lifter; it exists to validate the CPU model, flag model, memory model, ABI,
call lowering, indirect dispatch, segment handling, and ARM64 codegen on real
WSE2 code before the catalog work starts.

## Files

| File | Role |
|---|---|
| `lift.py` | Capstone -> C, strict subset, fails loudly on anything unimplemented |
| `cpu.h` | guest registers, flags, memory, SSE helpers |
| `harness.c` | native runner for the `memcmp` proof |
| `harness_sse.c` | native runner for the `setLocalScaling` proof |
| `harness_call.c` | native runner for the `std::string::compare` proof |
| `harness_indirect.c` | native runner for the vtable indirect-call proof |
| `diff.py` | Unicorn vs lifted C, `memcmp` |
| `diff_sse.py` | Unicorn vs lifted C, SSE + RIP-relative, compares XMM state |
| `diff_call.py` | Unicorn vs lifted C, direct call + stack frame + `cmov`/`setcc` |
| `diff_indirect.py` | Unicorn vs lifted C, vtable call through `wse2_dispatch_call` |
| `flagdiff.py` | synthesizes width/flag cases, lifts them, runs hardware vs lifted C |
| `flagdiff_main.c` | hardware-vs-lifted driver for `flagdiff.py` |
| `hw_x86.S` | Win64 trampoline that executes the original bytes on the host CPU |
| `seg_test.py` | GS/TEB addressing test; asserts FS overrides fail the lift |
| `requirements.txt` | pinned Python and Zig versions |
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
python lift.py $exe 0x281B50 lifted_queryinterface.c

& $zig cc -target x86_64-windows-gnu -O1 -o harness.exe harness.c lifted_memcmp.c
& $zig cc -target x86_64-windows-gnu -O1 -o harness_sse.exe harness_sse.c lifted_scaling.c
& $zig cc -target x86_64-windows-gnu -O1 -o harness_call.exe harness_call.c lifted_compare.c lifted_memcmp.c
& $zig cc -target x86_64-windows-gnu -O1 -o harness_indirect.exe harness_indirect.c lifted_queryinterface.c lifted_scaling.c

& $zig cc -target aarch64-linux-gnu -O2 -c lifted_memcmp.c -o lifted_memcmp_arm64.o
& $zig cc -target aarch64-linux-gnu -O2 -c lifted_scaling.c -o lifted_scaling_arm64.o
& $zig cc -target aarch64-linux-gnu -O2 -c lifted_compare.c -o lifted_compare_arm64.o
& $zig cc -target aarch64-linux-gnu -O2 -c lifted_queryinterface.c -o lifted_queryinterface_arm64.o

python diff.py $exe .\harness.exe .\work
python diff_sse.py $exe .\harness_sse.exe .\work_sse
python diff_call.py $exe .\harness_call.exe .\work_call
python diff_indirect.py $exe .\harness_indirect.exe .\work_indirect
python check_arm64.py

# hardware differential for operand widths and flags (Windows x64 host)
python flagdiff.py --zig $zig

# GS/TEB addressing and segment-override rejection
python seg_test.py --zig $zig
```

Expected:

```
vectors: 495  mismatches: 0
vectors: 464  mismatches: 0
vectors: 636  mismatches: 0
vectors: 464  mismatches: 0
total: 14200 vectors, 0 mismatches
gs_abs  rax=1122334455667788
gs_base rax=aabbccddeeff0011
fs override rejected: segment override fs: unsupported at 0x2000
```

The hardware differential compares all GPRs, the defined flags, and all 16
XMM registers. It caught 6,079 mismatches before the width fixes; the current
rig passes every vector.

## Model notes

- `CPU` carries `r[16]`, `flags`, `rip`, `teb` (the GS base), and `xmm[16]`
  (low 128 bits).
- Guest memory is one flat buffer. The proofs load the PE image at offset 0, so
  RVA-addressed data and RIP-relative operands resolve directly.
- `ret` pops the return address into `c->rip` and returns; the harness checks
  `rip` and `rsp` after every call.
- A direct `call` pushes the return RVA on the guest stack and becomes a C call
  to `f_<rva>`. The callee returns through the same guest stack.
- An indirect `call r/m64` evaluates the target first, pushes the return RVA,
  then calls `wse2_dispatch_call(c, target)`. The runtime table resolves a
  guest VA to a lifted function; an unregistered target aborts loudly.
- Flags cover CF, PF, AF, ZF, SF, OF. `adc`/`sbb` take an explicit carry/borrow
  input; CF is computed in 128-bit arithmetic. Shift counts are masked with
  `31` for 8/16/32-bit operands and `63` for 64-bit, and every shift writes
  the destination even at count zero so 32-bit writes clear bits 32-63.
  `imul` flags are computed at the operation width from sign-extended
  operands; only CF/OF are defined.
- `addr_expr` lowers `gs:` through `c->teb`. `fs:` and other overrides raise a
  fatal lift error; they are never silently ignored.
- `lift()` never catches an instruction-lowering exception. A missing opcode
  aborts the lift with the address and mnemonic.
- Immediates are truncated to the operation width. This matters: `sbb eax, -1`
  is `0xFFFFFFFF`, not `0xFFFFFFFFFFFFFFFF`.

## Scope

Implemented: `mov`, `movabs`, `movzx`, `movsx`, `movsxd`, `lea`, integer
add/sub/and/or/xor/cmp/test, `adc`, `sbb`, `inc`, `dec`, `neg`, `not`, shifts,
`imul` (2/3 operand), `bswap`, `setcc`, `cmovcc`, `jcc`, `jmp`, `call` (direct
and indirect via `wse2_dispatch_call`), `ret`, `push`, `pop`, `leave`, `cdqe`,
`cwde`, `cbw`, `xchg`, `bt/btc/btr/bts`, `movss`, `movsd`, `movaps`, `movdqa`,
`movups`, `movdqu`, `andps`, `orps`, `xorps`, `andnps`, `pand`, `por`, `pxor`,
`pandn`, `movd`, `movq`.

Not implemented, by design: x87, `div`/`idiv`, `rep` string ops, `syscall`,
`cpuid`, `rdtsc`, exceptions, imports. The lifter aborts with the address and
mnemonic rather than emitting a guess.

Note on proof 3: every path of `std::string::compare` ends in the same epilogue
`add rsp, 0x20`, so the final flag word is constant across vectors. The branch
semantics are validated through the return value (`-1`, `0`, `+1` all occur),
and the flag model itself was validated by proof 1.
