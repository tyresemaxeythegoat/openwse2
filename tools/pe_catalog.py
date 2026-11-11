#!/usr/bin/env python3
"""Build a function catalog for a PE32+ image from .pdata and public symbols.

The catalog is the input to the lifter: every row is one code extent with an
exact range, an optional name, and its UNWIND_INFO flags. `CHAININFO` rows are
cold fragments of another function, not separate functions: they carry a
`parent` RVA and are grouped under the owning primary row's `fragments` list.
Use `--functions-only` to emit one row per primary function.

Usage:
    python pe_catalog.py mb_warband_wse2_x64.exe \
        --symbols wse2_x64_pub.txt \
        --jsonl catalog.jsonl

    python pe_catalog.py mb_warband_wse2_x64.exe \
        --symbols wse2_x64_pub.txt \
        --functions-only --jsonl catalog-functions.jsonl

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


def unwind_info(pe, rows):
    """Decode UNWIND_INFO for every .pdata row.

    CHAININFO rows are cold fragments of another function. Their UNWIND_INFO
    ends with a RUNTIME_FUNCTION triple that points at the parent (or at
    another fragment), so we keep that triple for parent resolution.
    """
    rdata = next((s for s in pe.sections if s.Name.rstrip(b"\0") == b".rdata"), None)
    if rdata is None:
        return {}
    base = rdata.VirtualAddress
    img = rdata.get_data()
    out = {}
    for beg, end, unwind in rows:
        off = unwind - base
        if off < 0 or off + 4 > len(img):
            out[beg] = {"version": None, "flags": None, "handler": None, "chain": None}
            continue
        b0 = img[off]
        flags = (b0 >> 3) & 0x1F
        count = img[off + 2]
        # UNWIND_CODE entries are 2 bytes and the handler / chain field that
        # follows is 4-byte aligned, so an odd CountOfCodes has 2 bytes of
        # padding. Ignoring it shifts every handler RVA into nonsense.
        pos = off + 4 + count * 2
        if count & 1:
            pos += 2
        handler = None
        if flags & (UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER):
            if pos + 4 <= len(img):
                handler = struct.unpack_from("<I", img, pos)[0]
            pos += 4
        chain = None
        if flags & UNW_FLAG_CHAININFO and pos + 12 <= len(img):
            chain = struct.unpack_from("<III", img, pos)
        out[beg] = {
            "version": b0 & 7,
            "flags": flags,
            "handler": handler,
            "chain": chain,
        }
    return out


def primary_parent(beg, info):
    """Follow CHAININFO links to the owning primary function, if possible."""
    seen = set()
    cur = beg
    while cur not in seen:
        seen.add(cur)
        u = info.get(cur, {})
        chain = u.get("chain")
        if not chain:
            return None
        parent = chain[0]
        pu = info.get(parent)
        if pu is None:
            return parent
        if not ((pu.get("flags") or 0) & UNW_FLAG_CHAININFO):
            return parent
        cur = parent
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--symbols")
    ap.add_argument("--jsonl")
    ap.add_argument("--functions-only", action="store_true",
                    help="write only primary functions; exclude CHAININFO fragments")
    ap.add_argument("--summary", action="store_true")
    args = ap.parse_args()

    pe = pefile.PE(args.exe, fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = section_map(pe)
    rows = pdata_rows(pe)
    syms = load_symbols(args.symbols)
    info = unwind_info(pe, rows)

    text = secs.get(".text")
    in_text = 0
    covered = 0
    primary_bytes = 0
    functions = 0
    fragments = 0
    named = 0
    buckets = collections.Counter()
    flag_counts = collections.Counter()

    classified = []
    fragments_by_parent = collections.defaultdict(list)
    for beg, end, unwind in rows:
        u = info.get(beg, {})
        fl = u.get("flags")
        is_fragment = fl is not None and (fl & UNW_FLAG_CHAININFO)
        parent = primary_parent(beg, info) if is_fragment else None
        classified.append((beg, end, u, fl, is_fragment, parent))
        if is_fragment:
            fragments_by_parent[parent].append((beg, end - beg))

    primary_begs = {beg for beg, _end, _u, _fl, is_fragment, _parent in classified
                    if not is_fragment}
    unresolved = sum(1 for _beg, _end, _u, _fl, is_fragment, parent in classified
                     if is_fragment and parent not in primary_begs)

    out = open(args.jsonl, "w", encoding="utf-8", newline="\n") if args.jsonl else None

    for beg, end, u, fl, is_fragment, parent in classified:
        size = end - beg
        if text and text["rva"] <= beg and end <= text["rva"] + text["vsize"]:
            in_text += 1
            covered += size
        name = syms.get(beg)
        if name:
            named += 1
        if is_fragment:
            fragments += 1
        else:
            functions += 1
            primary_bytes += size
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

        if out and not (args.functions_only and is_fragment):
            row = {
                "rva": beg,
                "va": base + beg,
                "size": size,
                "name": name,
                "kind": "fragment" if is_fragment else "function",
                "unwind": u,
            }
            if is_fragment:
                row["parent"] = parent
            else:
                row["fragments"] = [
                    {"rva": frag_beg, "size": frag_size}
                    for frag_beg, frag_size in sorted(fragments_by_parent.get(beg, []))
                ]
            out.write(json.dumps(row, ensure_ascii=False) + "\n")

    if out:
        out.close()

    print("image base       0x%X" % base)
    print("entry            0x%X" % (base + pe.OPTIONAL_HEADER.AddressOfEntryPoint))
    print("pdata entries    %d" % len(rows))
    print("  primary funcs  %d" % functions)
    print("  CHAININFO      %d fragments" % fragments)
    if unresolved:
        print("  unresolved     %d fragments" % unresolved)
    print("in .text         %d" % in_text)
    print("covered bytes    %d" % covered)
    if text:
        print("text vsize       %d" % text["vsize"])
        print("coverage         %.2f%% (all extents)" % (100.0 * covered / text["vsize"]))
        print("primary coverage %.2f%%" % (100.0 * primary_bytes / text["vsize"]))
    print("named starts     %d" % named)
    print("unnamed starts   %d" % (len(rows) - named))
    print("unwind           " + ", ".join("%s=%d" % (k, v) for k, v in sorted(flag_counts.items())))
    print("size buckets     " + ", ".join("<%s:%d" % (k, v) for k, v in sorted(buckets.items(), key=lambda kv: int(kv[0]))))
    if args.jsonl:
        print("wrote            %s" % args.jsonl)

    return 0


if __name__ == "__main__":
    sys.exit(main())
