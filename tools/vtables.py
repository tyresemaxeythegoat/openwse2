#!/usr/bin/env python3
"""Scan .rdata/.data pointer runs pointing into .text and tag MSVC destructor-derived classes."""

import argparse
import json
import os
import struct

import pefile


def scan_vtables(exe_path, globals_path, min_entries=4):
    names = {}
    with open(globals_path, encoding="utf-8") as f:
        globals_rows = json.load(f)["globals"]
    for row in globals_rows:
        va = row.get("va")
        if va and row.get("kind") in ("public", "global", "local"):
            names.setdefault(va, row["name"])

    pe = pefile.PE(exe_path, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = {s.Name.rstrip(b"\0").decode("latin1"): s for s in pe.sections}
    text = secs[".text"]
    lo = base + text.VirtualAddress
    hi = lo + text.Misc_VirtualSize

    runs = []
    for sname in (".rdata", ".data"):
        sec = secs.get(sname)
        if sec is None:
            continue
        data = sec.get_data()
        n = len(data) // 8
        vals = struct.unpack_from("<%dQ" % n, data, 0)
        start = None
        for i, v in enumerate(vals + (0,)):
            in_text = lo <= v < hi
            if in_text and start is None:
                start = i
            if not in_text and start is not None:
                count = i - start
                if count >= min_entries:
                    runs.append({
                        "va": base + sec.VirtualAddress + start * 8,
                        "section": sname,
                        "entries": count,
                        "targets": list(vals[start:i]),
                    })
                start = None

    out = []
    for r in runs:
        first = r["targets"][0]
        fname = names.get(first, "")
        cls = None
        if fname.startswith("??_E") or fname.startswith("??_G"):
            cls = fname[4:].split("@@")[0]
        elif fname.startswith("??1"):
            cls = fname[3:].split("@@")[0]
        named = [names.get(t) for t in r["targets"]]
        out.append({
            "va": r["va"],
            "section": r["section"],
            "entries": r["entries"],
            "class_guess": cls,
            "first": fname,
            "named_entries": sum(1 for x in named if x),
            "targets": r["targets"],
            "names": named,
        })

    out.sort(key=lambda r: r["va"])
    return out


def main():
    ap = argparse.ArgumentParser(description="Scan PE .rdata/.data pointer runs for MSVC vtables.")
    ap.add_argument("exe", help="Path to PE32+ executable")
    ap.add_argument("--globals", required=True, help="Path to globals JSON from pdb_symbols.py")
    ap.add_argument("--out", required=True, help="Output vtables JSON path")
    ap.add_argument("--min-entries", type=int, default=4, help="Minimum contiguous .text pointers")
    args = ap.parse_args()

    out = scan_vtables(args.exe, args.globals, min_entries=args.min_entries)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"vtables": out}, f, indent=1)
        f.write("\n")

    cls = [r for r in out if r["class_guess"]]
    print("pointer runs >=%d: %d" % (args.min_entries, len(out)))
    print("with destructor-derived class: %d" % len(cls))


if __name__ == "__main__":
    main()
