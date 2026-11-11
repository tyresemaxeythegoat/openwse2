#!/usr/bin/env python3
"""Differential test for the indirect-call proof.

?QueryInterface@rglEffectStateManager (0x281B50) loads a vtable from [rcx] and
calls slot 1. The fake vtable points slot 1 at setLocalScaling (0x43E640), so
the original machine code and the lifted C both traverse an indirect call into
a second lifted function, with the guest return address pushed to the stack.

The vtable slot holds a full VA for Unicorn (image mapped at 0x140000000) and
an RVA for the lifted harness (image mapped flat at 0), which is exactly what
the lifter's --image-base option controls.
"""
import argparse
import os
import random
import subprocess
import sys

import pefile
import unicorn
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UcError
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8,
    UC_X86_REG_RSP, UC_X86_REG_RIP, UC_X86_REG_EFLAGS,
)

FUNC_RVA = 0x281B50
CALLEE_RVA = 0x43E640
BUF = 0x200000000
STACK = 0x300000000
SENT = 0xDEADB000
RET = SENT + 0xEEF

OBJ_OFF = 0x1000
VT_OFF = 0x2000
VEC_OFF = 0x3000
PPV_OFF = 0x4000

CF, PF, ZF, SF, OF = 1, 4, 64, 128, 2048
MASK = CF | PF | ZF | SF | OF


def gen_vectors():
    pool = [
        0x00000000, 0x80000000, 0x3F800000, 0xBF800000,
        0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
        0x7F7FFFFF, 0xFF7FFFFF, 0x00800000, 0x80800000,
        0x00000001, 0x80000001, 0x40000000, 0xC0000000,
        0x3EAAAAAB, 0xBEAAAAAB, 0x4B000000, 0xCB000000,
    ]
    cases = []
    for a in pool:
        cases.append((a, 0x3F800000, 0xBF800000))
        cases.append((a, 0x00000000, 0x80000000))
        cases.append((a, 0x7F7FFFFF, 0x00800000))
    for b in (0x00000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF):
        cases.append((b, b ^ 0x80000000, b ^ 0xFFFFFFFF))
    rnd = random.Random(0x1DD1)
    for _ in range(400):
        cases.append((rnd.getrandbits(32), rnd.getrandbits(32), rnd.getrandbits(32)))
    return cases


def write_vectors(cases, path):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for a, b, c in cases:
            f.write("%08x %08x %08x\n" % (a, b, c))


def run_unicorn(exe, cases, expected_path):
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    size = (len(img) + 0xFFF) & ~0xFFF
    mu = Uc(UC_ARCH_X86, UC_MODE_64)
    mu.mem_map(base, size)
    mu.mem_write(base, img)
    mu.mem_map(BUF, 0x10000)
    mu.mem_map(STACK, 0x20000)
    mu.mem_map(SENT, 0x1000)
    mu.mem_write(SENT + 0xEEF, b"\xF4")

    lines = []
    for a, b, c in cases:
        mu.mem_write(BUF, b"\x00" * 0x10000)
        obj = BUF + OBJ_OFF
        vt = BUF + VT_OFF
        vec = BUF + VEC_OFF
        ppv = BUF + PPV_OFF
        mu.mem_write(obj, vt.to_bytes(8, "little"))
        mu.mem_write(vt + 8, (base + CALLEE_RVA).to_bytes(8, "little"))
        mu.mem_write(vec, a.to_bytes(4, "little") + b.to_bytes(4, "little")
                     + c.to_bytes(4, "little") + b"\xEF\xBE\xAD\xDE")
        mu.mem_write(obj + 0x20, b"\xCC" * 16)
        sp = STACK + 0x10000 - 8
        mu.mem_write(sp, RET.to_bytes(8, "little"))
        mu.reg_write(UC_X86_REG_RSP, sp)
        mu.reg_write(UC_X86_REG_RAX, 0)
        mu.reg_write(UC_X86_REG_RCX, obj)
        mu.reg_write(UC_X86_REG_RDX, vec)
        mu.reg_write(UC_X86_REG_R8, ppv)
        mu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        try:
            mu.emu_start(base + FUNC_RVA, RET)
        except UcError as e:
            if not (e.errno == unicorn.UC_ERR_EXCEPTION and mu.reg_read(UC_X86_REG_RIP) == RET):
                raise
        eax = mu.reg_read(UC_X86_REG_RAX) & 0xFFFFFFFF
        efl = mu.reg_read(UC_X86_REG_EFLAGS)
        flags = 0
        for bit, name in ((0x001, CF), (0x004, PF), (0x040, ZF), (0x080, SF), (0x800, OF)):
            if efl & bit:
                flags |= name
        words = [int.from_bytes(mu.mem_read(obj + 0x20 + 4 * i, 4), "little")
                 for i in range(4)]
        ppv_ok = 1 if int.from_bytes(mu.mem_read(ppv, 8), "little") == obj else 0
        obj_ok = 1 if int.from_bytes(mu.mem_read(obj, 8), "little") == vt else 0
        lines.append("%x %x %u %u %x %x %x %x 1" % (
            eax, flags & MASK, ppv_ok, obj_ok, words[0], words[1], words[2], words[3]))
    with open(expected_path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("harness")
    ap.add_argument("workdir")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    image = os.path.join(args.workdir, "image.bin")
    vec = os.path.join(args.workdir, "vectors_indirect.txt")
    exp = os.path.join(args.workdir, "expected_indirect.txt")
    act = os.path.join(args.workdir, "actual_indirect.txt")

    pe = pefile.PE(args.exe, fast_load=True)
    with open(image, "wb") as f:
        f.write(pe.get_memory_mapped_image())

    cases = gen_vectors()
    write_vectors(cases, vec)
    run_unicorn(args.exe, cases, exp)
    subprocess.run([args.harness, image, vec, act], check=True)

    expected = open(exp, encoding="ascii").read().splitlines()
    actual = open(act, encoding="ascii").read().splitlines()
    if len(expected) != len(actual):
        print("COUNT MISMATCH: expected %d actual %d" % (len(expected), len(actual)))
        return 1

    bad = 0
    for k, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            bad += 1
            if bad <= 8:
                print("MISMATCH case %d vector=%08x %08x %08x" % (k, *cases[k]))
                print("  expected=%s" % e)
                print("  actual  =%s" % a)
    print("vectors: %d  mismatches: %d" % (len(cases), bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
