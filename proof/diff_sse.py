#!/usr/bin/env python3
"""Differential test for the SSE + RIP-relative proof function.

Runs btConvexInternalShape::setLocalScaling (0x43e640) in Unicorn and the
lifted C harness, then compares the 16-byte written object and the full XMM
register file.

The guest image is loaded flat at RVA 0 so the lifted code's absolute
RIP-relative target (0x4EFB10, the D3DX-style abs mask) resolves to the same
bytes the emulator sees at image_base + 0x4EFB10.
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
    UC_X86_REG_R9, UC_X86_REG_RSP, UC_X86_REG_RIP, UC_X86_REG_XMM0,
)

FUNC_RVA = 0x43E640
BUF = 0x200000000
STACK = 0x300000000
SENT = 0xDEADB000
RET = SENT + 0xEEF

INPUT_OFF = 0x0000
OBJ_OFF = 0x2000


def specials():
    return [
        0x00000000, 0x80000000, 0x3F800000, 0xBF800000,
        0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
        0x7F7FFFFF, 0xFF7FFFFF, 0x00800000, 0x80800000,
        0x00000001, 0x80000001, 0x40000000, 0xC0000000,
        0x3EAAAAAB, 0xBEAAAAAB, 0x4B000000, 0xCB000000,
    ]


def gen_vectors():
    pool = specials()
    cases = []
    # Every special in the x slot, paired with extremes in y and z.
    for a in pool:
        cases.append((a, 0x3F800000, 0xBF800000))
        cases.append((a, 0x00000000, 0x80000000))
        cases.append((a, 0x7F7FFFFF, 0x00800000))
    # Sign-only pairs, to exercise andps across every bit.
    for b in (0x00000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF):
        cases.append((b, b ^ 0x80000000, b ^ 0xFFFFFFFF))
    rnd = random.Random(0x5EED)
    for _ in range(400):
        cases.append((rnd.getrandbits(32), rnd.getrandbits(32), rnd.getrandbits(32)))
    return cases


def write_vectors(cases, path):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        for a, b, c in cases:
            f.write("%08x %08x %08x\n" % (a, b, c))


def fmt_case(mem16, xmms):
    out = mem16.hex()
    for q0, q1 in xmms:
        out += " %016x%016x" % (q0, q1)
    return out


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

    pattern = [bytes([0xA0 + i]) * 16 for i in range(16)]
    lines = []
    for a, b, c in cases:
        mu.mem_write(BUF, b"\x00" * 0x10000)
        mu.mem_write(BUF + INPUT_OFF, a.to_bytes(4, "little") + b.to_bytes(4, "little") + c.to_bytes(4, "little"))
        sp = STACK + 0x10000 - 8
        mu.mem_write(sp, RET.to_bytes(8, "little"))
        mu.reg_write(UC_X86_REG_RSP, sp)
        mu.reg_write(UC_X86_REG_RAX, 0)
        mu.reg_write(UC_X86_REG_RCX, BUF + OBJ_OFF)
        mu.reg_write(UC_X86_REG_RDX, BUF + INPUT_OFF)
        mu.reg_write(UC_X86_REG_R8, 0x1111111111111111)
        mu.reg_write(UC_X86_REG_R9, 0x2222222222222222)
        for i in range(16):
            mu.reg_write(UC_X86_REG_XMM0 + i, int.from_bytes(pattern[i], "little"))
        try:
            mu.emu_start(base + FUNC_RVA, RET)
        except UcError as e:
            if not (e.errno == unicorn.UC_ERR_EXCEPTION and mu.reg_read(UC_X86_REG_RIP) == RET):
                raise
        mem16 = bytes(mu.mem_read(BUF + OBJ_OFF + 0x20, 16))
        xmms = []
        for i in range(16):
            raw = int(mu.reg_read(UC_X86_REG_XMM0 + i)).to_bytes(16, "little")
            xmms.append((int.from_bytes(raw[0:8], "little"), int.from_bytes(raw[8:16], "little")))
        lines.append(fmt_case(mem16, xmms))
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
    vec = os.path.join(args.workdir, "vectors_sse.txt")
    exp = os.path.join(args.workdir, "expected_sse.txt")
    act = os.path.join(args.workdir, "actual_sse.txt")

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
                eparts, aparts = e.split(), a.split()
                print("MISMATCH case %d vector=%08x %08x %08x" % (k, *cases[k]))
                print("  out  expected=%s actual=%s" % (eparts[0], aparts[0]))
                for i, (ep, ap) in enumerate(zip(eparts[1:], aparts[1:])):
                    if ep != ap:
                        print("  xmm%-2d expected=%s actual=%s" % (i, ep, ap))
    print("vectors: %d  mismatches: %d" % (len(cases), bad))
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
