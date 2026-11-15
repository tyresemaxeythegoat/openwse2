#!/usr/bin/env python3
"""Verify that every v1.1.5.5 PE virtual address and REA evidence hash cited in C++ headers resolves cleanly.

Default mode uses committed artifacts only (docs/SYMBOL_MAP.json, docs/native_opcodes.json,
docs/wse_opcodes.json) so CI runs deterministically from a clean checkout.
Optional --exe / --globals / --old-globals flags validate against raw PE/PDB artifacts locally.
"""

import argparse
import bisect
import glob
import json
import os
import re
import struct
import sys

RE_FULL_VA = re.compile(r"(?:0x|FUN_|DAT_|PTR_|LAB_|SWITCH_|caseD_)(14[0-9a-fA-F]{7})\b")
RE_SHORT_VA = re.compile(r"\b0x(14[0-9a-fA-F]{7})((?:/[0-9a-fA-F]{3}\b)+)")
RE_RANGE_END = re.compile(r"\b0x14[0-9a-fA-F]{7}\.\.0x(14[0-9a-fA-F]{7})\b")
RE_EVIDENCE_ID = re.compile(r"\b(ev_[0-9a-fA-F]{8,64}(?:\.\.\.[0-9a-fA-F]+)?)")

# Superseded 2026-10-14 REA exports listed in docs/ARCHITECTURE.md / STATUS.md §7.
SUPERSEDED_EVIDENCE_PREFIXES = (
    "ev_1665f9ad",
    "ev_db4cc10a",
    "ev_7cd99ccc",
    "ev_8d91b175",
    "ev_25469ff4",
    "ev_e3530f99",
    "ev_cebc7ae0",
    "ev_24eecfa4",
    "ev_130d8023",
)


# Living prose documents scanned by --docs (AGENTS.md section 4). Their VAs are
# checked the same way as header VAs, except that (a) a line carrying
# DOC_IGNORE_MARKER is skipped (used to quote a corrected, wrong citation) and
# (b) VAs listed in DOCS_BASELINE (pre-existing image-base, .data, and labeled
# historical citations that SYMBOL_MAP.json does not curate) are tolerated, so
# only newly introduced unresolved citations fail.
DOC_FILES = (
    "AGENTS.md",
    "STATUS.md",
    "ROADMAP.md",
    "engine/README.md",
    "docs/ARCHITECTURE.md",
    "docs/PLATFORM_PLAN.md",
)
DOC_IGNORE_MARKER = "citation-check: ignore-line"
DOCS_BASELINE = os.path.join("tools", "docs_citation_baseline.txt")


def parse_hex(s):
    return int(s, 16)


def extract_doc_citations(repo_root):
    """Yield (relpath, line_no, va_int) for every full VA in the living docs."""
    refs = []
    for rel in DOC_FILES:
        path = os.path.join(repo_root, rel)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as f:
            for line_no, line in enumerate(f, 1):
                if DOC_IGNORE_MARKER in line:
                    continue
                for m in RE_FULL_VA.finditer(line):
                    refs.append((rel, line_no, parse_hex(m.group(1))))
    return refs


def load_docs_baseline(repo_root):
    path = os.path.join(repo_root, DOCS_BASELINE)
    allowed = set()
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                rel, va = line.split()
                allowed.add((rel, parse_hex(va)))
    return allowed


def extract_citations(include_root):
    """Yield (relpath, line_no, va_int, is_range_end) and evidence errors."""
    headers = sorted(glob.glob(os.path.join(include_root, "**", "*.h"), recursive=True))
    va_refs = []
    ev_errors = []
    for path in headers:
        rel = os.path.relpath(path).replace("\\", "/")
        with open(path, encoding="utf-8") as f:
            for line_no, line in enumerate(f, 1):
                range_ends = {parse_hex(m.group(1)) for m in RE_RANGE_END.finditer(line)}
                for m in RE_FULL_VA.finditer(line):
                    va = parse_hex(m.group(1))
                    va_refs.append((rel, line_no, va, va in range_ends))
                for m in RE_SHORT_VA.finditer(line):
                    base_hex = m.group(1)
                    for part in m.group(2).split("/"):
                        if not part:
                            continue
                        va = parse_hex(base_hex[:-len(part)] + part)
                        va_refs.append((rel, line_no, va, False))
                for m in RE_EVIDENCE_ID.finditer(line):
                    ev = m.group(1).lower()
                    if any(ev.startswith(p) for p in SUPERSEDED_EVIDENCE_PREFIXES):
                        ev_errors.append((rel, line_no, ev, "superseded 2026-10-14 REA run"))
    return headers, va_refs, ev_errors


class CitationIndex:
    def __init__(self, repo_root, exe_path=None, globals_path=None, old_globals_path=None):
        sym_path = os.path.join(repo_root, "docs", "SYMBOL_MAP.json")
        sym_map = json.load(open(sym_path, encoding="utf-8"))

        self.exact_vas = {}
        for row in sym_map.get("symbols", []):
            if "va" in row:
                self.exact_vas.setdefault(parse_hex(row["va"]), row["name"])
        for row in sym_map.get("strings", []):
            self.exact_vas.setdefault(parse_hex(row["va"]), "string:" + row["text"])
        for row in sym_map.get("tables", []):
            self.exact_vas.setdefault(parse_hex(row["va"]), "table:" + row["name"])
        for row in sym_map.get("vtables", []):
            self.exact_vas.setdefault(parse_hex(row["va"]), "vtable:" + row["class"])

        self._load_opcode_vas(os.path.join(repo_root, "docs", "native_opcodes.json"))
        self._load_opcode_vas(os.path.join(repo_root, "docs", "wse_opcodes.json"))

        self.func_ranges = sorted(
            (parse_hex(r["start"]), parse_hex(r["end"]), r["symbol"])
            for r in sym_map.get("function_ranges", [])
        )
        self.range_starts = [r[0] for r in self.func_ranges]

        self.global_objs = sorted(
            (parse_hex(g["va"]), parse_hex(g["end"]), g["name"])
            for g in sym_map.get("global_objects", [])
        )
        self.obj_starts = [o[0] for o in self.global_objs]

        self.superseded_vas = {parse_hex(x) for x in sym_map.get("superseded_vas", [])}

        # Optional deep local validation against retail v1.1.5.5 artifacts.
        self.pdata_ranges = []
        self.pdata_starts = []
        self.raw_globals = {}
        self.raw_superseded = set()

        if globals_path:
            rows = json.load(open(globals_path, encoding="utf-8"))["globals"]
            for row in rows:
                va = row.get("va")
                if va:
                    self.raw_globals.setdefault(va, set()).add(row["name"])
                    self.exact_vas.setdefault(va, row["name"])

        if exe_path:
            import pefile

            pe = pefile.PE(exe_path, fast_load=True)
            pe.parse_data_directories(
                directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
            )
            base = pe.OPTIONAL_HEADER.ImageBase
            raw_pdata = []
            for e in pe.DIRECTORY_ENTRY_EXCEPTION:
                raw_pdata.append((base + e.struct.BeginAddress, base + e.struct.EndAddress))
            raw_pdata.sort()
            self.pdata_ranges = raw_pdata
            self.pdata_starts = [s for s, _ in raw_pdata]

        if old_globals_path:
            raw = json.load(open(old_globals_path, encoding="utf-8"))
            old_rows = raw["globals"] if isinstance(raw, dict) else raw
            for row in old_rows:
                va = row.get("va")
                if not va:
                    continue
                if va not in self.exact_vas:
                    names = self.raw_globals.get(va, set())
                    if row["name"] not in names:
                        self.raw_superseded.add(va)

    def _load_opcode_vas(self, path):
        if not os.path.exists(path):
            return
        data = json.load(open(path, encoding="utf-8"))
        for key in ("jump_table", "table", "metadata"):
            val = data.get(key)
            if isinstance(val, dict):
                for subk, subv in val.items():
                    if isinstance(subv, str) and subv.startswith("0x14"):
                        self.exact_vas.setdefault(parse_hex(subv), "%s.%s" % (key, subk))
        for op in data.get("opcodes", []):
            for field in ("handler_va", "va", "site_va"):
                v = op.get(field)
                if isinstance(v, str) and v.startswith("0x14"):
                    self.exact_vas.setdefault(parse_hex(v), op.get("name", "opcode"))

    @staticmethod
    def _contains(ranges, starts, va, inclusive_end=False):
        idx = bisect.bisect_right(starts, va)
        lo_i = max(0, idx - 32)
        for s, e, label in ranges[lo_i:idx]:
            if s <= va and (va <= e if inclusive_end else va < e):
                return label
        return None

    def check_va(self, va, is_range_end=False):
        if va in self.superseded_vas or va in self.raw_superseded:
            return False, "superseded 2026-10-14 VA (no v1.1.5.5 symbol match at this address)"

        if va in self.exact_vas:
            return True, self.exact_vas[va]

        label = self._contains(self.func_ranges, self.range_starts, va, inclusive_end=is_range_end)
        if label:
            return True, "inside function " + label

        label = self._contains(self.global_objs, self.obj_starts, va, inclusive_end=False)
        if label:
            return True, "inside global " + label

        if self.pdata_ranges:
            idx = bisect.bisect_right(self.pdata_starts, va)
            lo_i = max(0, idx - 16)
            for s, e in self.pdata_ranges[lo_i:idx]:
                if s <= va and (va <= e if is_range_end else va < e):
                    return False, "in PE .pdata [0x%x..0x%x) but uncurated in docs/SYMBOL_MAP.json" % (s, e)

        return False, "unresolved VA (not in docs/SYMBOL_MAP.json or known opcode tables)"


def run_self_test(idx):
    # Negative tests: stale VA, wrong .rdata address, random unmapped VA, superseded ev_ prefix
    bad_cases = [
        (0x1402a1d00, "superseded FUN_1402a1d00"),
        (0x1405e9a20, "mis-cited ip2state 0x1405e9a20"),
        (0x140dead00, "bogus VA 0x140dead00"),
    ]
    for va, desc in bad_cases:
        ok, reason = idx.check_va(va)
        if ok:
            print("SELF-TEST FAIL: expected rejection for %s, got %s" % (desc, reason), file=sys.stderr)
            return False
    bad_ev = "ev_cebc7ae0bef7bccaddbcbb53c560d5d2020a5acf60c5bc03a77c496077e8c8d3"
    if not any(bad_ev.startswith(p) for p in SUPERSEDED_EVIDENCE_PREFIXES):
        print("SELF-TEST FAIL: expected superseded ev prefix match for %s" % bad_ev, file=sys.stderr)
        return False
    print("Self-test passed: rejected %d synthetic bad citations + superseded evidence ID." % len(bad_cases))
    return True


def main():
    repo_default = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Verify v1.1.5.5 VAs and evidence hashes cited in headers.")
    ap.add_argument("--repo-root", default=repo_default)
    ap.add_argument("--include-root", default=None, help="Header root (defaults to <repo-root>/engine/include)")
    ap.add_argument("--exe", help="Optional v1.1.5.5 PE32+ executable for local .pdata verification")
    ap.add_argument("--globals", help="Optional v1.1.5.5 globals JSON")
    ap.add_argument("--old-globals", help="Optional superseded globals JSON")
    ap.add_argument("--self-test", action="store_true", help="Also verify known-bad VAs are rejected")
    ap.add_argument("--docs", action="store_true",
                    help="Also check VAs in the living docs (DOC_FILES) against SYMBOL_MAP.json")
    ap.add_argument("--write-docs-baseline", action="store_true",
                    help="Rewrite %s with every currently unresolved doc VA and exit" % DOCS_BASELINE)
    args = ap.parse_args()

    include_root = args.include_root or os.path.join(args.repo_root, "engine", "include")
    idx = CitationIndex(args.repo_root, exe_path=args.exe, globals_path=args.globals, old_globals_path=args.old_globals)

    if args.self_test and not run_self_test(idx):
        return 1

    if args.write_docs_baseline:
        rows = sorted({(rel, va) for rel, _, va in extract_doc_citations(args.repo_root)
                       if not idx.check_va(va)[0]})
        with open(os.path.join(args.repo_root, DOCS_BASELINE), "w", encoding="utf-8", newline="\n") as f:
            f.write("# Pre-existing doc VAs not curated in docs/SYMBOL_MAP.json (image base, .data,\n")
            f.write("# labeled historical sections). Regenerate with --write-docs-baseline only after\n")
            f.write("# confirming each new entry is intentional. Format: <doc path> <VA>\n")
            for rel, va in rows:
                f.write("%s 0x%x\n" % (rel, va))
        print("Wrote %d baseline entries to %s" % (len(rows), DOCS_BASELINE))
        return 0

    headers, va_refs, ev_errors = extract_citations(include_root)
    failures = []
    doc_refs = []
    if args.docs:
        baseline = load_docs_baseline(args.repo_root)
        doc_refs = extract_doc_citations(args.repo_root)
        for rel, line_no, va in doc_refs:
            ok, reason = idx.check_va(va)
            if not ok and (rel, va) not in baseline:
                failures.append("%s:%d: 0x%x: %s" % (rel, line_no, va, reason))
    for rel, line_no, ev, reason in ev_errors:
        failures.append("%s:%d: %s: %s" % (rel, line_no, ev, reason))

    unique_vas = {}
    for rel, line_no, va, is_end in va_refs:
        ok, reason = idx.check_va(va, is_range_end=is_end)
        unique_vas[va] = ok
        if not ok:
            failures.append("%s:%d: 0x%x: %s" % (rel, line_no, va, reason))

    if failures:
        print("Citation check FAILED (%d errors):" % len(failures), file=sys.stderr)
        for msg in failures:
            print("  " + msg, file=sys.stderr)
        return 1

    print(
        "Citation check PASSED: %d headers, %d VA references (%d unique VAs), 0 superseded/unresolved citations."
        % (len(headers), len(va_refs), len(unique_vas))
    )
    if args.docs:
        print("Doc citation check PASSED: %d VA references across %d living docs." % (len(doc_refs), len(DOC_FILES)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
