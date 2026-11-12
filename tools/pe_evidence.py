#!/usr/bin/env python3
"""Extract static evidence (RTTI, strings, pointer tables) from a PE image.

This is Phase 1 triage tooling for the WSE2 reconstruction. It never executes
the target: it reads section bytes and reports virtual addresses that can be
cited next to Ghidra evidence.

Usage:
    python pe_evidence.py mb_warband_wse2_x64.exe --rtti
    python pe_evidence.py mb_warband_wse2_x64.exe --strings --filter script
    python pe_evidence.py mb_warband_wse2_x64.exe --ptrtables --json rtti.json

Every reported address is a full virtual address (image base included), which
is the coordinate space REA and Ghidra use for this image.
"""
import argparse
import collections
import json
import re
import struct
import sys

import pefile


def sections(pe):
    out = []
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode("latin1")
        out.append({
            "name": name,
            "rva": s.VirtualAddress,
            "vsize": s.Misc_VirtualSize,
            "raw_size": s.SizeOfRawData,
            "raw_ptr": s.PointerToRawData,
            "data": s.get_data(),
        })
    return out


def va_of(secs, base, rva):
    return base + rva


def section_for(secs, rva):
    for s in secs:
        if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["raw_size"]):
            return s
    return None


def rtti_name(mangled):
    """Turn an MSVC type-descriptor string into a display name.

    .?AVCTroop@@            -> CTroop
    .?AVFoo@Bar@@           -> Bar::Foo
    .?AV?$vector@V...@@     -> ?$vector@V... (templates stay mangled)
    """
    body = mangled[4:]
    if body.startswith("?$") or body.startswith("?A"):
        return mangled
    scopes = []
    for part in body.split("@"):
        if not part:
            break
        scopes.append(re.sub(r"^[0-9]+", "", part))
    if not scopes:
        return mangled
    scopes.reverse()
    return "::".join(scopes)


def scan_rtti(secs, base):
    rows = []
    rx = re.compile(rb"\.\?A[VUW][ -~]{1,400}?@@")
    for s in secs:
        if s["name"] not in (".rdata", ".data"):
            continue
        data = s["data"]
        for m in rx.finditer(data):
            mangled = m.group().decode("latin1")
            rva = s["rva"] + m.start()
            rows.append({
                "va": base + rva,
                "descriptor_va": base + rva - 16,
                "mangled": mangled,
                "name": rtti_name(mangled),
                "section": s["name"],
            })
    # A type descriptor string may also appear as a plain literal elsewhere;
    # only the descriptor copy has a type_info vftable 16 bytes before it.
    uniq = {}
    for r in rows:
        uniq.setdefault(r["descriptor_va"], r)
    return sorted(uniq.values(), key=lambda r: r["va"])


def scan_strings(secs, base, min_len, utf16):
    out = []
    for s in secs:
        if s["name"] not in (".rdata", ".data", ".text"):
            continue
        data = s["data"]
        if utf16:
            rx = re.compile((r"(?:[ -~]\x00){%d,}" % min_len).encode())
            for m in rx.finditer(data):
                text = m.group().decode("utf-16-le", errors="replace")
                out.append({"va": base + s["rva"] + m.start(), "text": text,
                            "encoding": "utf-16le", "section": s["name"]})
        else:
            rx = re.compile((r"[ -~]{%d,}" % min_len).encode())
            for m in rx.finditer(data):
                text = m.group().decode("latin1")
                out.append({"va": base + s["rva"] + m.start(), "text": text,
                            "encoding": "ascii", "section": s["name"]})
    return out


def scan_ptrtables(secs, base, min_run, sections_filter):
    text = next((s for s in secs if s["name"] == ".text"), None)
    if text is None:
        raise SystemExit("no .text section")
    lo, hi = base + text["rva"], base + text["rva"] + text["vsize"]
    runs = []
    for s in secs:
        if sections_filter and s["name"] not in sections_filter:
            continue
        if s["name"] not in (".rdata", ".data"):
            continue
        data = s["data"]
        n = len(data) // 8
        if n == 0:
            continue
        vals = struct.unpack_from("<%dQ" % n, data, 0)
        start = None
        count = 0
        for i, v in enumerate(vals):
            if lo <= v < hi:
                if start is None:
                    start = i
                count += 1
            else:
                if start is not None and count >= min_run:
                    runs.append((s, start, count, vals[start:start + count]))
                start = None
                count = 0
        if start is not None and count >= min_run:
            runs.append((s, start, count, vals[start:start + count]))
    rows = []
    for s, start, count, targets in runs:
        va = base + s["rva"] + start * 8
        rows.append({
            "va": va,
            "section": s["name"],
            "entries": count,
            "target_min": min(targets),
            "target_max": max(targets),
            # Stable ordering tells a vtable/dispatch table from a data blob.
            "sorted": all(targets[i] <= targets[i + 1] for i in range(len(targets) - 1)),
            "targets": list(targets),
        })
    rows.sort(key=lambda r: -r["entries"])
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--rtti", action="store_true")
    ap.add_argument("--strings", action="store_true")
    ap.add_argument("--ptrtables", action="store_true")
    ap.add_argument("--utf16", action="store_true")
    ap.add_argument("--min-len", type=int, default=5)
    ap.add_argument("--filter", default=None)
    ap.add_argument("--min-run", type=int, default=16)
    ap.add_argument("--ptr-sections", default=None,
                    help="comma-separated section list for pointer scans")
    ap.add_argument("--limit", type=int, default=200)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    pe = pefile.PE(args.exe, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = sections(pe)
    rx = re.compile(args.filter) if args.filter else None
    payload = {}

    if args.rtti:
        rows = scan_rtti(secs, base)
        if rx:
            rows = [r for r in rows if rx.search(r["mangled"])]
        payload["rtti"] = rows
        print("rtti type descriptors: %d" % len(rows))
        counts = collections.Counter(r["name"].split("::")[-1] for r in rows)
        for name, count in counts.most_common(min(args.limit, len(counts))):
            print("  %-40s %d" % (name, count))

    if args.strings:
        rows = scan_strings(secs, base, args.min_len, args.utf16)
        if rx:
            rows = [r for r in rows if rx.search(r["text"])]
        payload["strings"] = rows
        print("strings (%s): %d" % ("utf-16" if args.utf16 else "ascii", len(rows)))
        for r in rows[:args.limit]:
            print("  0x%x  %s" % (r["va"], r["text"][:160]))

    if args.ptrtables:
        filt = args.ptr_sections.split(",") if args.ptr_sections else None
        rows = scan_ptrtables(secs, base, args.min_run, filt)
        payload["ptrtables"] = [{k: v for k, v in r.items() if k != "targets"} | {
            "first_targets": ["0x%x" % t for t in r["targets"][:8]]}
            for r in rows]
        print("pointer runs (>=%d): %d" % (args.min_run, len(rows)))
        for r in rows[:args.limit]:
            print("  0x%x  %-7s entries=%-5d targets=0x%x..0x%x sorted=%s" % (
                r["va"], r["section"], r["entries"], r["target_min"],
                r["target_max"], r["sorted"]))

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, indent=1)
            f.write("\n")
        print("wrote %s" % args.json)

    pe.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
