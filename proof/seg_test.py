#!/usr/bin/env python3
"""GS-relative addressing test for the proof lifter.

x86-64 keeps the GS base outside the GPR file. The lifter must lower
`gs:[...]` through the guest TEB and must refuse FS or any other segment
override instead of silently reading the wrong address.

Usage:
    python seg_test.py --zig path\\to\\zig.exe
"""
import argparse
import os
import subprocess
import sys

import capstone

import lift

CASES = [
    ("gs_abs", "65488b0425 30000000".replace(" ", ""), 0x1000, 0x30),
    ("gs_base", "65488b00", 0x1010, 0x1000),
]


def disasm_one(code, rva):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    insns = list(md.disasm(code, rva))
    if sum(i.size for i in insns) != len(code):
        raise SystemExit("disassembly did not consume 0x%X" % rva)
    return insns


def lift_case(name, hx, rva, workdir):
    raw = bytes.fromhex(hx) + b"\xC3"
    insns = disasm_one(raw, rva)
    text = lift.Lifter(insns, rva, rva + len(raw)).lift()
    if "c->teb" not in text:
        raise SystemExit("%s: lifted code does not use c->teb" % name)
    path = os.path.join(workdir, "seg_%s.c" % name)
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    return path, insns[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zig", required=True)
    ap.add_argument("--workdir", default="work_seg")
    args = ap.parse_args()
    os.makedirs(args.workdir, exist_ok=True)

    lifted = []
    for name, hx, rva, _disp in CASES:
        path, insn = lift_case(name, hx, rva, args.workdir)
        lifted.append(path)
        print("lifted %-8s %s %s" % (name, insn.mnemonic, insn.op_str))

    # FS must fail the lift, not silently drop the override.
    fs_raw = bytes.fromhex("64488b042530000000")
    fs_insns = disasm_one(fs_raw, 0x2000)
    try:
        lift.Lifter(fs_insns, 0x2000, 0x2000 + len(fs_raw)).lift()
    except SystemExit as e:
        if "segment override fs" not in str(e):
            raise SystemExit("fs test failed with unexpected error: %s" % e)
        print("fs override rejected:", e)
    else:
        raise SystemExit("fs override was accepted; expected a fatal lift error")

    harness = os.path.join(args.workdir, "seg_harness.c")
    with open(harness, "w", encoding="ascii", newline="\n") as f:
        f.write("""#include "cpu.h"

uint32_t f_1000(CPU *c);
uint32_t f_1010(CPU *c);

static uint8_t membuf[MEM_SIZE];
uint8_t *MEM = membuf;

int main(void) {
    CPU c;
    memset(&c, 0, sizeof c);
    c.teb = 0x1000;
    c.r[4] = 0x8000;
    st64(0x1030, 0x1122334455667788ull);
    st64(0x1000, 0xAABBCCDDEEFF0011ull);
    st64(0x8000, 0xDEADBEEFull);

    uint32_t rc = f_1000(&c);
    if (rc != 0 || c.r[0] != 0x1122334455667788ull) {
        printf("gs_abs FAIL rc=%u rax=%llx\\n", rc,
               (unsigned long long)c.r[0]);
        return 1;
    }
    printf("gs_abs  rax=%llx\\n", (unsigned long long)c.r[0]);

    memset(&c, 0, sizeof c);
    c.teb = 0x1000;
    c.r[0] = 0;
    c.r[4] = 0x8000;
    st64(0x1000, 0xAABBCCDDEEFF0011ull);
    st64(0x8000, 0xDEADBEEFull);
    rc = f_1010(&c);
    if (rc != 0 || c.r[0] != 0xAABBCCDDEEFF0011ull) {
        printf("gs_base FAIL rc=%u rax=%llx\\n", rc,
               (unsigned long long)c.r[0]);
        return 1;
    }
    printf("gs_base rax=%llx\\n", (unsigned long long)c.r[0]);
    return 0;
}
""")

    exe = os.path.abspath(os.path.join(args.workdir, "seg_harness.exe"))
    cmd = [
        args.zig, "cc", "-target", "x86_64-windows-gnu", "-O1",
        "-I.", "-o", exe, harness,
        os.path.abspath(lifted[0]), os.path.abspath(lifted[1]),
    ]
    subprocess.run(cmd, check=True)
    return subprocess.run([exe]).returncode


if __name__ == "__main__":
    sys.exit(main())
