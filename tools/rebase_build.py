#!/usr/bin/env python3
"""Rebase the repository's evidence from one WSE2 x64 build to the next (local only).

Every WSE2 release ships `mb_warband_wse2_x64.exe` with its PDB. Given the old and new pair,
this tool:

  1. generates the PDB/PE artifacts for both builds into --work (outside the repository:
     retail-derived data is never committed): globals, classes, layouts, vtables;
  2. maps every v-old VA cited in the repository to the new build:
       entry      the VA is a PDB symbol (function or data) that exists by name in the new build;
       same       an interior VA in a function whose instruction shape is unchanged;
       aligned    an interior VA mapped through an instruction-level alignment of the old and
                  new bodies of its symbol (difflib over mnemonic + operands with constants
                  masked), so a bug fix only affects VAs next to it;
       unresolved anything else (listed in --work/report.md for manual review);
  3. checks that the curated classes keep their TPI type index and size;
  4. with --apply: rewrites the mapped VAs in tracked text files (not in generated opcode
     files, legacy directories, or lines marked as superseded/historical), updates the TARGET
     block of tools/build_symbol_map.py, and regenerates docs/SYMBOL_MAP.json with the old
     build's VAs recorded as superseded.

Usage (from the repository root, never from %TEMP%\\opencode\\wse2_pc):
  python tools/rebase_build.py --old-exe OLD.exe --old-pdb OLD.pdb \\
      --new-exe NEW.exe --new-pdb NEW.pdb --release v1.1.5.6 --work <dir> [--apply]

Requires pefile and capstone. `docs/wse_opcodes.json` and its two siblings are generated from
Ghidra pseudocode (tools/extract_wse_opcodes.py) and must be regenerated separately.
"""

from __future__ import annotations

import argparse
import bisect
import datetime
import difflib
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
VA_RE = re.compile(r"0x14[0-9a-fA-F]{7}\b")
MASK_RE = re.compile(r"0x[0-9a-f]+")
# Generated from Ghidra pseudocode: regenerate, never rewrite.
GENERATED = {"docs/wse_opcodes.json", "docs/WSE_OPCODES.md", "engine/include/wse/vm/wse_opcodes.h",
             "docs/SYMBOL_MAP.json",
             # its self-test embeds deliberately wrong VAs
             "tools/check_citations.py", "tools/rebase_build.py"}
LEGACY_PREFIXES = ("analysis/", "proof/", "runtime/")
SKIP_LINE_RE = re.compile(r"superseded|2026-10-14|historical|citation-check: ignore-line", re.I)
TEXT_SUFFIXES = {".h", ".hpp", ".cpp", ".c", ".md", ".py", ".txt", ".kt", ".kts", ".ps1", ".sh",
                 ".yml", ".json", ".cmake"}


def run(cmd: list[str]) -> None:
    print("  $", " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, cwd=ROOT, stdout=subprocess.DEVNULL)


def generate(exe: Path, pdb: Path, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    py = sys.executable
    if not (out / "globals.json").exists():
        run([py, TOOLS / "pdb_symbols.py", pdb, "--globals", "--exe", exe, "--json", out / "globals.json"])
    if not (out / "types.json").exists():
        run([py, TOOLS / "pdb_symbols.py", pdb, "--classes", "--json", out / "types.json"])
    layouts = out / "layouts"
    layouts.mkdir(exist_ok=True)
    sys.path.insert(0, str(TOOLS))
    import build_symbol_map as bsm  # noqa: E402

    for cls in bsm.CLASSES:
        target = layouts / bsm.LAYOUT_FILES.get(cls, cls + ".json")
        if not target.exists():
            run([py, TOOLS / "pdb_symbols.py", pdb, "--layout", cls, "--json", target])
    if not (out / "vtables.json").exists():
        run([py, TOOLS / "vtables.py", exe, "--globals", out / "globals.json", "--out", out / "vtables.json"])


class Build:
    def __init__(self, exe: Path, globals_json: Path):
        self.pe = pefile.PE(str(exe))
        self.img = self.pe.get_memory_mapped_image()
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.ranges = sorted((self.base + e.struct.BeginAddress, self.base + e.struct.EndAddress)
                             for e in self.pe.DIRECTORY_ENTRY_EXCEPTION)
        self.starts = [r[0] for r in self.ranges]
        self.by_va: dict[int, str] = {}
        self.by_name: dict[str, int] = {}
        for s in json.load(open(globals_json, encoding="utf-8"))["globals"]:
            if s.get("va"):
                self.by_va.setdefault(s["va"], s["name"])
                self.by_name.setdefault(s["name"], s["va"])
        self.sym_vas = sorted(self.by_va)

    def func_of(self, va: int):
        i = bisect.bisect_right(self.starts, va) - 1
        if i >= 0 and self.ranges[i][0] <= va < self.ranges[i][1]:
            return self.ranges[i]
        return None

    def sym_at_or_before(self, va: int):
        i = bisect.bisect_right(self.sym_vas, va) - 1
        return self.sym_vas[i] if i >= 0 else None

    def span(self, sv: int):
        i = bisect.bisect_right(self.sym_vas, sv)
        end = self.sym_vas[i] if i < len(self.sym_vas) else sv + 0x1000
        return sv, min(end, sv + 0x200000)


MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def stream(b: Build, s: int, e: int):
    code = b.img[s - b.base:e - b.base]
    out = []
    off = 0
    while off < len(code):
        got = False
        for ins in MD.disasm(code[off:], s + off):
            out.append((ins.address, ins.mnemonic + " " + MASK_RE.sub("#", ins.op_str)))
            off = ins.address - s + ins.size
            got = True
        if not got or off < len(code):
            off += 1  # resync past an undecodable byte (capstone stops at invalid padding)
    return out


class Mapper:
    def __init__(self, old: Build, new: Build):
        self.old, self.new = old, new
        self.cache: dict[tuple[int, int], dict[int, int]] = {}
        self.same_cache: dict[tuple[int, int], bool] = {}

    def unchanged(self, so: int, sn: int) -> bool:
        """Same .pdata size and the same masked instruction sequence (computed once per pair)."""
        key = (so, sn)
        if key not in self.same_cache:
            ofr, nfr = self.old.func_of(so), self.new.func_of(sn)
            same = False
            if ofr and nfr and (ofr[1] - ofr[0]) == (nfr[1] - nfr[0]):
                a = [x[1] for x in stream(self.old, *ofr)]
                same = bool(a) and a == [x[1] for x in stream(self.new, *nfr)]
            self.same_cache[key] = same
        return self.same_cache[key]

    def aligned(self, so: int, sn: int) -> dict[int, int]:
        key = (so, sn)
        if key not in self.cache:
            a = stream(self.old, *self.old.span(so))
            b = stream(self.new, *self.new.span(sn))
            sm = difflib.SequenceMatcher(None, [x[1] for x in a], [x[1] for x in b], autojunk=False)
            table = {}
            for tag, i1, i2, j1, _j2 in sm.get_opcodes():
                if tag == "equal":
                    for k in range(i2 - i1):
                        table[a[i1 + k][0]] = b[j1 + k][0]
            self.cache[key] = table
        return self.cache[key]

    def map(self, va: int):
        old, new = self.old, self.new
        if va == old.base and old.base == new.base:
            return va, "entry", "image base"
        name = old.by_va.get(va)
        if name is not None and name in new.by_name:
            return new.by_name[name], "entry", name
        if old.func_of(va) is None:
            hit = self.data_offset(va)
            if hit is not None:
                return hit, "data-offset", old.by_va.get(old.sym_at_or_before(va))
            hit = self.signature(va)
            if hit is not None:
                return hit, "signature", old.by_va.get(old.sym_at_or_before(va))
            return None, "unresolved", "no symbol at this VA and not inside a .pdata function"
        sv = old.sym_at_or_before(va)
        name = old.by_va.get(sv)
        if name is None or name not in new.by_name:
            return None, "unresolved", f"enclosing symbol {name!r} not in the new build"
        nv = new.by_name[name]
        if self.unchanged(sv, nv):
            return nv + (va - sv), "same", name
        hit = self.aligned(sv, nv).get(va)
        if hit is not None:
            return hit, "aligned", name
        hit = self.gap_match(sv, nv, va)
        if hit is not None:
            return hit, "gap", name
        hit = self.signature(va)
        if hit is not None:
            return hit, "signature", name
        return None, "unresolved", f"inside changed code of {name}"

    def data_offset(self, va: int):
        """Unnamed data inside a named object that keeps its size in the new build."""
        old, new = self.old, self.new
        sv = old.sym_at_or_before(va)
        name = old.by_va.get(sv) if sv is not None else None
        if name is None or name not in new.by_name:
            return None
        i = bisect.bisect_right(old.sym_vas, sv)
        old_size = (old.sym_vas[i] - sv) if i < len(old.sym_vas) else None
        nv = new.by_name[name]
        j = bisect.bisect_right(new.sym_vas, nv)
        new_size = (new.sym_vas[j] - nv) if j < len(new.sym_vas) else None
        if old_size is None or old_size != new_size or va - sv >= old_size:
            return None
        return nv + (va - sv)

    def signature(self, va: int, min_len: int = 6, max_len: int = 40):
        """Decode from `va` in the old build; find the unique offset in the new body of the same
        symbol where the same masked instruction sequence decodes (lengthened until unique)."""
        old, new = self.old, self.new
        sv = old.sym_at_or_before(va)
        name = old.by_va.get(sv) if sv is not None else None
        if name is None or name not in new.by_name:
            return None

        def seq(b: Build, at: int, n: int):
            code = b.img[at - b.base:at - b.base + 16 * n]
            out = []
            for ins in MD.disasm(code, at):
                out.append(ins.mnemonic + " " + MASK_RE.sub("#", ins.op_str))
                if len(out) == n or ins.mnemonic in ("ret", "jmp", "int3"):
                    break
            return out

        nv = new.by_name[name]
        s, e = new.span(nv)
        want = seq(old, va, max_len)
        if len(want) < min_len and not (want and want[-1].startswith(("ret", "jmp"))):
            return None
        starts = list(range(s, e))
        n = min(min_len, len(want))
        while True:
            target = want[:n]
            starts = [p for p in starts if seq(new, p, n) == target]
            if len(starts) <= 1 or n >= len(want):
                break
            n = min(n + 6, len(want))
        return starts[0] if len(starts) == 1 else None

    def gap(self, so: int, sn: int, va: int):
        """Old and new instructions between the nearest aligned neighbours around `va`."""
        a = stream(self.old, *self.old.span(so))
        b = stream(self.new, *self.new.span(sn))
        table = self.aligned(so, sn)
        idx = next((i for i, x in enumerate(a) if x[0] == va), None)
        if idx is None:
            return None
        lo = next((i for i in range(idx - 1, -1, -1) if a[i][0] in table), None)
        hi = next((i for i in range(idx + 1, len(a)) if a[i][0] in table), None)
        new_index = {x[0]: i for i, x in enumerate(b)}
        jlo = new_index[table[a[lo][0]]] + 1 if lo is not None else 0
        jhi = new_index[table[a[hi][0]]] if hi is not None else len(b)
        olo = lo + 1 if lo is not None else 0
        ohi = hi if hi is not None else len(a)
        return a[olo:ohi], b[jlo:jhi], a[idx][1]

    def gap_match(self, so: int, sn: int, va: int):
        """`va`'s instruction text occurs exactly once in the new gap (and once in the old)."""
        g = self.gap(so, sn, va)
        if g is None:
            return None
        old_gap, new_gap, text = g
        if sum(1 for x in old_gap if x[1] == text) != 1:
            return None
        hits = [x[0] for x in new_gap if x[1] == text]
        return hits[0] if len(hits) == 1 else None


def tracked_text_files() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    files = []
    for f in out.split():
        if f in GENERATED or f.startswith(LEGACY_PREFIXES) or Path(f).suffix not in TEXT_SUFFIXES:
            continue
        files.append(f)
    return files


def cited_vas(files: list[str]) -> dict[int, list[str]]:
    seen: dict[int, list[str]] = {}
    for f in files:
        try:
            lines = (ROOT / f).read_text(encoding="utf-8").splitlines()
        except (UnicodeDecodeError, OSError):
            continue
        for n, line in enumerate(lines, 1):
            if SKIP_LINE_RE.search(line):
                continue
            for m in VA_RE.findall(line):
                seen.setdefault(int(m, 16), []).append(f"{f}:{n}")
    return seen


def rewrite(files: list[str], mapping: dict[int, int]) -> int:
    changed = 0

    def sub(m: re.Match) -> str:
        v = int(m.group(0), 16)
        if v not in mapping:
            return m.group(0)
        text = "0x%x" % mapping[v]
        return text.upper().replace("0X", "0x") if m.group(0)[2:].isupper() else text

    for f in files:
        p = ROOT / f
        try:
            raw = p.read_bytes()
            text = raw.decode("utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        crlf = "\r\n" in text
        lines = text.replace("\r\n", "\n").split("\n")
        new_lines = [line if SKIP_LINE_RE.search(line) else VA_RE.sub(sub, line) for line in lines]
        if new_lines != lines:
            out = "\n".join(new_lines)
            p.write_bytes((out.replace("\n", "\r\n") if crlf else out).encode("utf-8"))
            changed += 1
    return changed


def codeview(pe: pefile.PE):
    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []):
        e = d.entry
        if e is not None and hasattr(e, "Signature_Data1"):
            guid = "%08x-%04x-%04x-%s-%s" % (e.Signature_Data1, e.Signature_Data2, e.Signature_Data3,
                                            bytes(e.Signature_Data4[:2]).hex(), bytes(e.Signature_Data4[2:]).hex())
            return guid, e.Age
    return None, None


def file_version(pe: pefile.PE) -> str | None:
    for fi in getattr(pe, "FileInfo", []) or []:
        for entry in fi:
            for st in getattr(entry, "StringTable", []):
                v = st.entries.get(b"FileVersion")
                if v:
                    return v.decode(errors="replace")
    return None


def update_target(new_exe: Path, release: str) -> None:
    pe = pefile.PE(str(new_exe))
    guid, age = codeview(pe)
    sha = hashlib.sha256(new_exe.read_bytes()).hexdigest()
    path = TOOLS / "build_symbol_map.py"
    src = path.read_text(encoding="utf-8")
    repl = {
        "release": release,
        "file_version": file_version(pe) or release.lstrip("v"),
        "sha256": sha,
        "pdb_guid": guid,
    }
    for key, value in repl.items():
        src, n = re.subn(r'("%s"\s*:\s*)"[^"]*"' % key, lambda m: m.group(1) + '"%s"' % value, src, count=1)
        if n != 1:
            raise SystemExit(f"TARGET key {key!r} not found in build_symbol_map.py")
    src, n = re.subn(r'("pdb_age"\s*:\s*)\d+', lambda m: m.group(1) + str(age), src, count=1)
    if n != 1:
        raise SystemExit("TARGET key 'pdb_age' not found in build_symbol_map.py")
    path.write_text(src, encoding="utf-8", newline="\n")


def write_packets(out: Path, mapper: Mapper, unresolved) -> None:
    """One review file per unresolved VA: citing lines, old/new code around it (local only)."""
    out.mkdir(parents=True, exist_ok=True)
    old, new = mapper.old, mapper.new
    for va, why, where in unresolved:
        lines = [f"# {hex(va)}", "", f"Reason: {why}", "", "## Cited at", ""]
        for w in where[:6]:
            f, n = w.rsplit(":", 1)
            try:
                text = (ROOT / f).read_text(encoding="utf-8").splitlines()[int(n) - 1].strip()
            except (OSError, IndexError, UnicodeDecodeError):
                text = ""
            lines.append(f"- `{w}`: {text[:300]}")
        sv = old.sym_at_or_before(va)
        name = old.by_va.get(sv) if sv else None
        lines += ["", f"Nearest old symbol at or before: `{name}` at {hex(sv) if sv else None} (+{hex(va - sv) if sv else '?'})"]
        nv = new.by_name.get(name) if name else None
        lines.append(f"Same symbol in the new build: {hex(nv) if nv else 'absent'}")
        if nv is not None and old.func_of(va) is not None:
            g = mapper.gap(sv, nv, va)
            if g is not None:
                old_gap, new_gap, text = g
                lines += ["", f"Instruction at the VA (constants masked): `{text}`", "",
                          "## Old build, between the nearest aligned instructions", "```"]
                lines += [f"{hex(a)}  {t}" for a, t in old_gap[:120]]
                lines += ["```", "", "## New build, the corresponding gap", "```"]
                lines += [f"{hex(a)}  {t}" for a, t in new_gap[:120]]
                lines += ["```"]
        else:
            for label, b in (("old", old), ("new", new)):
                secs = [(s.Name.rstrip(b"\0").decode(), b.base + s.VirtualAddress, s.Misc_VirtualSize)
                        for s in b.pe.sections]
                lines.append(f"{label} sections: " + ", ".join(f"{n} {hex(a)}+{hex(z)}" for n, a, z in secs))
        (out / f"{va:x}.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old-exe", required=True, type=Path)
    ap.add_argument("--old-pdb", required=True, type=Path)
    ap.add_argument("--new-exe", required=True, type=Path)
    ap.add_argument("--new-pdb", required=True, type=Path)
    ap.add_argument("--release", required=True, help="new release label, e.g. v1.1.5.6")
    ap.add_argument("--work", required=True, type=Path, help="scratch directory outside the repository")
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()
    work = args.work.resolve()
    if ROOT in work.parents or work == ROOT:
        raise SystemExit("--work must be outside the repository (retail-derived data)")

    print("generating artifacts")
    generate(args.old_exe, args.old_pdb, work / "old")
    generate(args.new_exe, args.new_pdb, work / "new")

    old_types = {r["name"]: r for r in json.load(open(work / "old/types.json"))["classes"]}
    new_types = {r["name"]: r for r in json.load(open(work / "new/types.json"))["classes"]}
    curated = json.load(open(ROOT / "docs/SYMBOL_MAP.json", encoding="utf-8"))["classes"]
    tpi_changes = []
    for name in curated:
        o, n = old_types.get(name), new_types.get(name)
        if not n or not o or o["type_index"] != n["type_index"] or o["size"] != n["size"]:
            tpi_changes.append((name, o and (o["type_index"], o["size"]), n and (n["type_index"], n["size"])))

    print("mapping cited VAs")
    old = Build(args.old_exe, work / "old/globals.json")
    new = Build(args.new_exe, work / "new/globals.json")
    mapper = Mapper(old, new)
    files = tracked_text_files()
    cites = cited_vas(files)
    mapping: dict[int, int] = {}
    counts: dict[str, int] = {}
    unresolved = []
    for va in sorted(cites):
        nv, cls, why = mapper.map(va)
        counts[cls] = counts.get(cls, 0) + 1
        if nv is None:
            unresolved.append((va, why, cites[va]))
        elif nv != va:
            mapping[va] = nv
    (work / "va_map.json").write_text(json.dumps({hex(k): hex(v) for k, v in mapping.items()}, indent=0))
    write_packets(work / "packets", mapper, unresolved)

    report = [f"# Rebase report: {args.release}", "",
              f"Cited VAs: {len(cites)}; " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())), "",
              f"Curated classes with a changed TPI index or size: {len(tpi_changes)}"]
    report += [f"- {n}: {o} -> {nn}" for n, o, nn in tpi_changes]
    report += ["", "## Unresolved VAs (review by hand)", ""]
    report += [f"- `{hex(va)}`: {why} ({', '.join(where[:3])}{' ...' if len(where) > 3 else ''})"
               for va, why, where in unresolved]
    (work / "report.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    print("\n".join(report[:4]))
    print(f"report: {work / 'report.md'}")

    if args.apply:
        if tpi_changes:
            print("WARNING: TPI indices or sizes changed; review report.md before trusting layouts")
        n = rewrite(files, mapping)
        print(f"rewrote VAs in {n} files")
        update_target(args.new_exe, args.release)
        run([sys.executable, TOOLS / "build_symbol_map.py",
             "--layouts", work / "new/layouts", "--types", work / "new/types.json",
             "--globals", work / "new/globals.json", "--vtables", work / "new/vtables.json",
             "--exe", args.new_exe, "--pdb", args.new_pdb, "--old-globals", work / "old/globals.json",
             "--generated", datetime.date.today().isoformat(), "--out", ROOT / "docs/SYMBOL_MAP.json"])
        print("regenerated docs/SYMBOL_MAP.json; now run tools/check_citations.py --self-test --docs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
