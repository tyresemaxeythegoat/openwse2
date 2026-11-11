#!/usr/bin/env python3
"""Build a function catalog for a PE32+ image from .pdata and public symbols.

The catalog is the input to the lifter: every row is one function with an exact
extent, an optional name, and its UNWIND_INFO flags.

Usage:
    python pe_catalog.py mb_warband_wse2_x64.exe \
        --symbols wse2_x64_pub.txt \
        --jsonl catalog.jsonl

Symbol file format: one entry per line, either

    <hex-rva> <name>
    <hex-rva> <size> <name>

which is what `llvm-pdbutil dump -publics` output reduces to.
"""
import argparse
import collections
import json
import struct
import sys

import pefile

# UNWIND_INFO flag bits (Windows x64).
UNW_FLAG_EHANDLER = 0x1
UNW_FLAG_UHANDLER = 0x2
UNW_FLAG_CHAININFO = 0x4


def load_symbols(path):
    syms = {}
    if not path:
        return syms
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.rstrip("\n").split(" ", 2)
            if len(parts) < 2:
                continue
            try:
                rva = int(parts[0], 16)
            except ValueError:
                continue
            name = parts[-1].strip()
            if name and rva not in syms:
                syms[rva] = name
    return syms


def section_map(pe):
    out = {}
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode("latin1")
        out[name] = {
            "name": name,
            "rva": s.VirtualAddress,
            "vsize": s.Misc_VirtualSize,
            "raw_size": s.SizeOfRawData,
        }
    return out


def pdata_rows(pe):
    sec = next((s for s in pe.sections if s.Name.rstrip(b"\0") == b".pdata"), None)
    if sec is None:
        raise SystemExit("no .pdata section: not an x64 image?")
    data = sec.get_data()
    rows = []
    for i in range(len(data) // 12):
        beg, end, unwind = struct.unpack_from("<III", data, i * 12)
        if end <= beg:
            continue
        rows.append((beg, end, unwind))
    return rows


def unwind_flags(pe, rows):
    rdata = next((s for s in pe.sections if s.Name.rstrip(b"\0") == b".rdata"), None)
    if rdata is None:
        return {}
    base = rdata.VirtualAddress
    img = rdata.get_data()
    out = {}
    for beg, end, unwind in rows:
        off = unwind - base
        if off < 0 or off + 4 > len(img):
            out[beg] = {"version": None, "flags": None}
            continue
        b0 = img[off]
        out[beg] = {"version": b0 & 7, "flags": (b0 >> 3) & 0x1F}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--symbols")
    ap.add_argument("--jsonl")
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()

    pe = pefile.PE(args.exe, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = section_map(pe)
    rows = pdata_rows(pe)
    syms = load_symbols(args.symbols)
    flags = unwind_flags(pe, rows)

    text = secs.get(".text")
    in_text = 0
    covered = 0
    named = 0
    buckets = collections.Counter()
    flag_counts = collections.Counter()
    out = open(args.jsonl, "w", encoding="utf-8", newline="\n") if args.jsonl else None

    for beg, end, unwind in rows:
        size = end - beg
        if text and text["rva"] <= beg and end <= text["rva"] + text["vsize"]:
            in_text += 1
            covered += size
        name = syms.get(beg)
        if name:
            named += 1
        u = flags.get(beg, {})
        fl = u.get("flags")
        if fl is not None:
            if fl & UNW_FLAG_EHANDLER:
                flag_counts["ehandler"] += 1
            if fl & UNW_FLAG_UHANDLER:
                flag_counts["uhandler"] += 1
            if fl & UNW_FLAG_CHAININFO:
                flag_counts["chaininfo"] += 1
            if fl == 0:
                flag_counts["none"] += 1
        b = 16
        for edge in (16, 32, 64, 128, 256, 512, 1024, 4096, 65536):
            b = edge
            if size < edge:
                break
        buckets[str(b)] += 1

        if out:
            out.write(json.dumps({
                "rva": beg,
                "va": base + beg,
                "size": size,
                "name": name,
                "unwind": u,
            }, ensure_ascii=False) + "\n")

    if out:
        out.close()

    print("image base       0x%X" % base)
    print("entry            0x%X" % (base + pe.OPTIONAL_HEADER.AddressOfEntryPoint))
    print("pdata entries    %d" % len(rows))
    print("in .text         %d" % in_text)
    print("covered bytes    %d" % covered)
    if text:
        print("text vsize       %d" % text["vsize"])
        print("coverage         %.2f%%" % (100.0 * covered / text["vsize"]))
    print("named starts     %d" % named)
    print("unnamed starts   %d" % (len(rows) - named))
    print("unwind           " + ", ".join("%s=%d" % (k, v) for k, v in sorted(flag_counts.items())))
    print("size buckets     " + ", ".join("<%s:%d" % (k, v) for k, v in sorted(buckets.items(), key=lambda kv: int(kv[0]))))
    if args.jsonl:
        print("wrote            %s" % args.jsonl)

    return 0


if __name__ == "__main__":
    sys.exit(main())
