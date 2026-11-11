#!/usr/bin/env python3
"""Summarize a PDB public-symbol dump for an MSVC C++ image.

Input is a reduced dump, one line per symbol:

    <hex-rva> <name>
    <hex-rva> <size> <name>

Produce it from the shipped PDB with:

    llvm-pdbutil dump -publics mb_warband_wse2_x64.pdb \
      | awk '/S_PUB32/ { ... }'

or any tool that yields section-relative addresses plus decorated names.

Reports symbol count, namespace/class distribution, and the leading
namespaces, which is how the shim surface is prioritized.
"""
import argparse
import collections
import re
import sys


def parse(path):
    rows = []
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
            if name:
                rows.append((rva, name))
    return rows


def decorated_class(name):
    """Extract the innermost named scope from an MSVC decorated name.

    ?method@Class@@...        -> Class
    ?method@Ns@Class@@...     -> Class
    ??0Class@@...             -> Class
    ?$vector@V...@std@@...    -> std
    """
    body = name.lstrip("?")
    parts = body.split("@")
    first = re.sub(r"^[0-9]+", "", parts[0])

    scopes = []
    for part in parts[1:]:
        if not part:
            break
        scopes.append(part)
    if scopes:
        inner = scopes[-1]
        clean = inner
        # Strip MSVC type/literal encodings that prefix a scope name.
        for prefix in ("$$CB", "$$", "$", "CB"):
            if clean.startswith(prefix):
                clean = clean[len(prefix):]
                break
        if len(clean) > 1 and clean[0] in "VUT":
            clean = clean[1:]
        return clean or first
    return first or "(global)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("symbols")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--filter", default=None, help="regex over symbol names")
    args = ap.parse_args()

    rows = parse(args.symbols)
    rx = re.compile(args.filter) if args.filter else None
    if rx:
        rows = [(a, n) for a, n in rows if rx.search(n)]

    print("symbols           %d" % len(rows))
    addrs = [a for a, _ in rows]
    if addrs:
        print("rva range         0x%X .. 0x%X" % (min(addrs), max(addrs)))
    dupes = len(addrs) - len(set(addrs))
    print("duplicate rvas    %d" % dupes)

    classes = collections.Counter(decorated_class(n) for _, n in rows)
    print("\ntop classes:")
    for name, count in classes.most_common(args.top):
        print("  %-32s %d" % (name, count))

    kinds = collections.Counter()
    for _, n in rows:
        if n.startswith("??0"):
            kinds["constructor"] += 1
        elif n.startswith("??1"):
            kinds["destructor"] += 1
        elif n.startswith("??_"):
            kinds["operator"] += 1
        elif n.startswith("?"):
            kinds["method/function"] += 1
        else:
            kinds["c symbol"] += 1
    print("\nkinds:")
    for name, count in kinds.most_common():
        print("  %-20s %d" % (name, count))

    return 0


if __name__ == "__main__":
    sys.exit(main())
