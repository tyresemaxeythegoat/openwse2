#!/usr/bin/env python3
"""Differential test: original x86-64 code in Unicorn vs lifted C.

Generates deterministic memcmp vectors, executes the original WSE2/CRT memcmp
under Unicorn, runs the lifted C harness, and compares EAX and the defined
integer flags.
"""
import argparse
import os
import random
import struct
import subprocess
import sys

import pefile
import unicorn
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UcError
from unicorn.x86_const import (
    UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9,
    UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_RSP, UC_X86_REG_RBP,
    UC_X86_REG_EFLAGS, UC_X86_REG_RIP,
)

FUNC_RVA = 0x485390
BUF = 0x200000000
STACK = 0x300000000
SENT = 0xDEADB000
RET = SENT + 0xEEF

CF, PF, ZF, SF, OF = 1, 4, 64, 128, 2048
MASK = CF | PF | ZF | SF | OF


def gen_vectors():
    cases = []
    lengths = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 33, 47, 63, 64, 65, 79, 80]
    for n in lengths:
        z = bytes(n)
        cases.append((z, z))
        for pos in sorted({0, n // 2, n - 1}):
            if n == 0:
                continue
            a = bytearray(n)
            b = bytearray(n)
            b[pos] ^= 0x01
            cases.append((bytes(a), bytes(b)))
            a = bytearray(n)
            b = bytearray(n)
            a[pos] = 0x80
            b[pos] = 0x7F
            cases.append((bytes(a), bytes(b)))
        cases.append((bytes([0x00]) * n, bytes([0xFF]) * n))
        cases.append((bytes([0xAA]) * n, bytes([0x55]) * n))
    rnd = random.Random(0xC0DE)
    for _ in range(300):
        n = rnd.randrange(0, 81)
        a = bytes(rnd.getrandbits(8) for _ in range(n))
        mode = rnd.randrange(5)
        if mode == 0:
            b = bytearray(a)
        elif mode == 1 and n:
            b = bytearray(a)
            b[rnd.randrange(n)] ^= 1 << rnd.randrange(8)
        elif mode == 2 and n:
            b = bytearray(a)
            b[rnd.randrange(n)] = rnd.getrandbits(8)
        elif mode == 3:
            b = bytearray(rnd.getrandbits(8) for _ in range(n))
        else:
            b = bytearray(a)
            for k in range(n):
                if rnd.randrange(4) == 0:
                    b[k] ^= 1 << rnd.randrange(8)
        cases.append((a, bytes(b)))
    return cases


def write_vectors(cases, path):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for a, b in cases:
            f.write("%d %s %s\n" % (len(a), a.hex() or "-", b.hex() or "-"))


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

    out = []
    for a, b in cases:
        mu.mem_write(BUF, b"\x00" * 0x10000)
        mu.mem_write(BUF + 0x1000, a)
        mu.mem_write(BUF + 0x1100, b)
        sp = STACK + 0x10000 - 8
        mu.mem_write(sp, struct.pack("<Q", RET))
        mu.reg_write(UC_X86_REG_RSP, sp)
        mu.reg_write(UC_X86_REG_RBP, 0)
        mu.reg_write(UC_X86_REG_RAX, 0)
        mu.reg_write(UC_X86_REG_RCX, BUF + 0x1000)
        mu.reg_write(UC_X86_REG_RDX, BUF + 0x1100)
        mu.reg_write(UC_X86_REG_R8, len(a))
        mu.reg_write(UC_X86_REG_R9, 0x123456789ABCDEF0)
        mu.reg_write(UC_X86_REG_R10, 0xFEDCBA9876543210)
        mu.reg_write(UC_X86_REG_R11, 0x0F0F0F0F0F0F0F0F)
        mu.reg_write(UC_X86_REG_EFLAGS, 0x202)
        try:
            mu.emu_start(base + FUNC_RVA, RET)
        except UcError as e:
            # Reaching the hlt sentinel is a clean stop; anything else is real.
            if not (e.errno == unicorn.UC_ERR_EXCEPTION and mu.reg_read(UC_X86_REG_RIP) == RET):
                raise
        eax = mu.reg_read(UC_X86_REG_RAX) & 0xFFFFFFFF
        efl = mu.reg_read(UC_X86_REG_EFLAGS)
        flags = 0
        if efl & 0x001:
            flags |= CF
        if efl & 0x004:
            flags |= PF
        if efl & 0x040:
            flags |= ZF
        if efl & 0x080:
            flags |= SF
        if efl & 0x800:
            flags |= OF
        out.append((eax, flags & MASK))
    with open(expected_path, "w", encoding="ascii", newline="\n") as f:
        for eax, flags in out:
            f.write("%x %x\n" % (eax, flags))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("harness")
    ap.add_argument("workdir")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    vec = os.path.join(args.workdir, "vectors.txt")
    exp = os.path.join(args.workdir, "expected.txt")
    act = os.path.join(args.workdir, "actual.txt")

    cases = gen_vectors()
    write_vectors(cases, vec)
    run_unicorn(args.exe, cases, exp)
    subprocess.run([args.harness, vec, act], check=True)

    expected = [line.split() for line in open(exp, encoding="ascii")]
    actual = [line.split() for line in open(act, encoding="ascii")]
    if len(expected) != len(actual):
        print("COUNT MISMATCH: expected %d actual %d" % (len(expected), len(actual)))
        return 1

    bad = 0
    for k, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            bad += 1
            if bad <= 12:
                print("MISMATCH case %d len=%d: expected eax=%s flags=%s actual eax=%s flags=%s" % (
                    k, len(cases[k][0]), e[0], e[1], a[0], a[1]))
    print("vectors: %d  mismatches: %d" % (len(cases), bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
