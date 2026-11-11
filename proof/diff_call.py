#!/usr/bin/env python3
"""Differential test for std::string::compare (0x124a0) calling memcmp.

The original code runs in Unicorn; the lifted caller and lifted callee run as
C. Both set up the same MSVC std::string objects and compare the return value
and the arithmetic flags.
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

FUNC_RVA = 0x124A0
BUF = 0x200000000
STACK = 0x300000000
SENT = 0xDEADB000
RET = SENT + 0xEEF

OBJ_A, OBJ_B = 0x1000, 0x2000
EXT_A, EXT_B = 0x4000, 0x5000

CF, PF, ZF, SF, OF = 1, 4, 64, 128, 2048
MASK = CF | PF | ZF | SF | OF


def gen_vectors():
    cases = []
    alphabet = b"abcde0123456789"

    def make(n, seed):
        rnd = random.Random(seed)
        return bytes(alphabet[rnd.randrange(len(alphabet))] for _ in range(n))

    lengths = [0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32, 40, 48]
    for na in lengths:
        for nb in (0, 1, na - 1 if na else 0, na, na + 1, 16, 17, 33):
            if nb < 0 or nb > 64:
                continue
            a = make(na, 1000 + na * 97 + nb)
            b = make(nb, 2000 + na * 89 + nb)
            cases.append((a, b))
            if nb > 0:
                cases.append((a, a[:nb]))
                pref = bytearray(a[:nb])
                if len(pref) == nb:
                    pref[-1] ^= 1
                    cases.append((a, bytes(pref)))
                else:
                    cases.append((a, bytes(pref) + b[len(pref):nb]))
        # Prefix relations and single-byte divergence.
        for pos in (0, na // 2, na - 1):
            if na == 0:
                continue
            a = make(na, 3000 + na + pos)
            b = bytearray(a)
            b[pos] ^= 0x01
            cases.append((a, bytes(b)))
            b = bytearray(a)
            b[pos] = 0x00 if b[pos] else 0x7F
            cases.append((a, bytes(b)))

    rnd = random.Random(0xCA11)
    for _ in range(200):
        na = rnd.randrange(0, 49)
        nb = rnd.randrange(0, 49)
        a = make(na, rnd.randrange(1 << 30))
        if rnd.randrange(2):
            b = make(nb, rnd.randrange(1 << 30))
        else:
            b = a[:nb] if nb <= na else a + make(nb - na, rnd.randrange(1 << 30))
            if nb and rnd.randrange(2):
                bb = bytearray(b)
                bb[rnd.randrange(nb)] ^= 1 << rnd.randrange(8)
                b = bytes(bb)
        cases.append((a, b))
    return cases


def write_vectors(cases, path):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for a, b in cases:
            f.write("%s %s\n" % (a.hex() or "-", b.hex() or "-"))


def build_string(mu, obj, ext, data):
    mu.mem_write(obj, b"\x00" * 0x20)
    if len(data) < 16:
        mu.mem_write(obj, data)
        mu.mem_write(obj + 0x10, len(data).to_bytes(8, "little"))
        mu.mem_write(obj + 0x18, (15).to_bytes(8, "little"))
    else:
        mu.mem_write(obj, ext.to_bytes(8, "little"))
        mu.mem_write(ext, data)
        mu.mem_write(obj + 0x10, len(data).to_bytes(8, "little"))
        mu.mem_write(obj + 0x18, len(data).to_bytes(8, "little"))


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
    for a, b in cases:
        mu.mem_write(BUF, b"\x00" * 0x10000)
        build_string(mu, BUF + OBJ_A, BUF + EXT_A, a)
        build_string(mu, BUF + OBJ_B, BUF + EXT_B, b)
        sp = STACK + 0x10000 - 8
        mu.mem_write(sp, RET.to_bytes(8, "little"))
        mu.reg_write(UC_X86_REG_RSP, sp)
        mu.reg_write(UC_X86_REG_RAX, 0)
        mu.reg_write(UC_X86_REG_RCX, BUF + OBJ_A)
        mu.reg_write(UC_X86_REG_RDX, BUF + OBJ_B)
        mu.reg_write(UC_X86_REG_R8, 0x1111111111111111)
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
        lines.append("%x %x" % (eax, flags & MASK))
    with open(expected_path, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("harness")
    ap.add_argument("workdir")
    args = ap.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    vec = os.path.join(args.workdir, "vectors_call.txt")
    exp = os.path.join(args.workdir, "expected_call.txt")
    act = os.path.join(args.workdir, "actual_call.txt")

    cases = gen_vectors()
    write_vectors(cases, vec)
    run_unicorn(args.exe, cases, exp)
    subprocess.run([args.harness, vec, act], check=True)

    expected = [l.split() for l in open(exp, encoding="ascii")]
    actual = [l.split() for l in open(act, encoding="ascii")]
    if len(expected) != len(actual):
        print("COUNT MISMATCH: expected %d actual %d" % (len(expected), len(actual)))
        return 1

    bad = 0
    for k, (e, a) in enumerate(zip(expected, actual)):
        if e != a:
            bad += 1
            if bad <= 10:
                print("MISMATCH case %d lenA=%d lenB=%d expected=%s actual=%s" % (
                    k, len(cases[k][0]), len(cases[k][1]), " ".join(e), " ".join(a)))
    print("vectors: %d  mismatches: %d" % (len(cases), bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
