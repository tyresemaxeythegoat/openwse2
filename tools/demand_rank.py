#!/usr/bin/env python3
"""Measure static opcode demand across Warband module text files.

Parses user-supplied `scripts.txt`, `triggers.txt`, and/or `simple_triggers.txt`
using the token grammar verified against mbScriptManager / mbTriggerManager /
mbSimpleTriggerManager, joins against `docs/native_opcodes.json` and
`docs/wse_opcodes.json`, checks live implementation coverage in `block_vm.h`
and `engine/include/wse/vm/**/*.h`, and prints aggregate counts only (never module names or
content).
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


OPCODE_MASK = 0x0FFFFFFF
ENTITY_ITERATOR_STUBS: set[int] = set()


class TokenCursor:
    def __init__(self, tokens: list[str], label: str) -> None:
        self.tokens = tokens
        self.label = label
        self.pos = 0

    def next_str(self) -> str:
        if self.pos >= len(self.tokens):
            raise SystemExit(f"{self.label}: unexpected EOF at token {self.pos}")
        tok = self.tokens[self.pos]
        self.pos += 1
        return tok

    def next_int(self) -> int:
        return int(self.next_str())

    def skip(self, count: int) -> None:
        if count < 0 or self.pos + count > len(self.tokens):
            raise SystemExit(f"{self.label}: truncated stream skipping {count} tokens at {self.pos}")
        self.pos += count

    def assert_eof(self) -> None:
        if self.pos != len(self.tokens):
            raise SystemExit(
                f"{self.label}: {len(self.tokens) - self.pos} trailing tokens unconsumed"
            )


def read_statement_block(cur: TokenCursor) -> list[int]:
    count = cur.next_int()
    if count < 0:
        raise SystemExit(f"{cur.label}: negative statement count {count}")
    opcodes: list[int] = []
    for _ in range(count):
        raw_op = cur.next_int()
        num_ops = cur.next_int()
        if num_ops < 0 or num_ops > 32:
            raise SystemExit(f"{cur.label}: operand count out of range ({num_ops})")
        cur.skip(num_ops)
        opcodes.append(raw_op & OPCODE_MASK)
    return opcodes


def parse_scripts_file(path: Path) -> list[list[int]]:
    tokens = path.read_text(encoding="utf-8", errors="replace").split()
    cur = TokenCursor(tokens, str(path))
    cur.skip(3)  # e.g. scriptsfile version 1
    count = cur.next_int()
    blocks: list[list[int]] = []
    for _ in range(count):
        cur.next_str()  # script id (discarded)
        cur.next_str()  # interval
        blocks.append(read_statement_block(cur))
    cur.assert_eof()
    return blocks


def parse_triggers_file(path: Path) -> list[list[int]]:
    tokens = path.read_text(encoding="utf-8", errors="replace").split()
    cur = TokenCursor(tokens, str(path))
    cur.skip(3)  # triggersfile version 1
    count = cur.next_int()
    records: list[list[int]] = []
    for _ in range(count):
        cur.skip(3)  # check, delay, rearm intervals
        cond = read_statement_block(cur)
        cons = read_statement_block(cur)
        records.append(cond + cons)
    cur.assert_eof()
    return records


def parse_simple_triggers_file(path: Path) -> list[list[int]]:
    tokens = path.read_text(encoding="utf-8", errors="replace").split()
    cur = TokenCursor(tokens, str(path))
    cur.skip(3)  # simple_triggers_file version 1
    count = cur.next_int()
    records: list[list[int]] = []
    for _ in range(count):
        cur.next_str()  # interval
        records.append(read_statement_block(cur))
    cur.assert_eof()
    return records


def scan_implemented_native(block_vm_path: Path) -> set[int]:
    paths = [block_vm_path] + sorted(block_vm_path.parent.glob("native_*.h"))

    texts = [p.read_text(encoding="utf-8", errors="replace") for p in paths]
    core_map: dict[str, int] = {}
    after_core_chunks: list[str] = []
    for text in texts:
        last_end = 0
        for core_m in re.finditer(
            r"namespace\s+core_op\s*\{(.*?)\}\s*//\s*namespace\s+core_op", text, re.S
        ):
            for m in re.finditer(
                r"k([A-Za-z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)U?", core_m.group(1)
            ):
                core_map["k" + m.group(1)] = int(m.group(2), 0)
            last_end = core_m.end()
        if last_end:
            after_core_chunks.append(text[last_end:])

    if not core_map:
        raise SystemExit(f"Could not locate core_op namespace in {block_vm_path}")

    disp_pattern = re.compile(
        r"dispatch_native(?:_[a-z0-9_]+)?_statement(?:_ext)?\s*\([^)]*\)\s*\{(.*?)\n\s*(?:\+\+ctx\.unimplementedCount;|return\s+(?:NativeDispatch::unimplemented|dispatch_native(?:_[a-z0-9_]+)?_statement(?:_ext)?\([^;]*\));)",
        re.S,
    )
    impl: set[int] = set()
    found_dispatch = False
    for text in texts:
        for disp_m in disp_pattern.finditer(text):
            found_dispatch = True
            disp_body = disp_m.group(1)
            for m in re.finditer(r"case\s+core_op::(k[A-Za-z0-9_]+)\s*:", disp_body):
                if m.group(1) in core_map:
                    impl.add(core_map[m.group(1)])
            for m in re.finditer(r"case\s+(0x[0-9A-Fa-f]+|\d+)U?\s*:", disp_body):
                impl.add(int(m.group(1), 0))

    if not found_dispatch:
        raise SystemExit(f"Could not locate dispatch_native_statement in {block_vm_path}")

    after_core = "\n".join(after_core_chunks)
    for name, op in core_map.items():
        if op < 30 and re.search(rf"\bcore_op::{re.escape(name)}\b", after_core):
            impl.add(op)
    return impl


def scan_implemented_wse(wse_header: Path, execute_wse: Path) -> set[int]:
    wh = wse_header.read_text(encoding="utf-8", errors="replace")
    enums = {
        m.group(1): int(m.group(2))
        for m in re.finditer(r"^\s*([A-Za-z0-9_]+)\s*=\s*(\d+)\s*,?", wh, re.M)
    }
    vm_root = execute_wse.parent if execute_wse.is_file() else execute_wse
    cases: set[str] = set()
    for hdr in sorted(vm_root.rglob("*.h")):
        eh = hdr.read_text(encoding="utf-8", errors="replace")
        cases.update(re.findall(r"case\s+WseOpcode::([A-Za-z0-9_]+)\s*:", eh))
    return {enums[c] for c in cases if c in enums}


def analyze_demand(
    records_by_kind: dict[str, list[list[int]]],
    native_map_path: Path,
    wse_map_path: Path,
    block_vm_path: Path,
    wse_header_path: Path,
    execute_wse_path: Path,
    top_n: int,
) -> dict:
    native_doc = json.loads(native_map_path.read_text(encoding="utf-8"))
    wse_doc = json.loads(wse_map_path.read_text(encoding="utf-8"))

    native_entries = {e["opcode"]: e for e in native_doc["entries"]}
    wse_entries = {e["opcode"]: e for e in wse_doc["entries"]}

    impl_native = scan_implemented_native(block_vm_path)
    impl_wse = scan_implemented_wse(wse_header_path, execute_wse_path)

    call_counts: Counter[int] = Counter()
    record_presence: dict[int, int] = Counter()
    per_kind_summary: dict[str, dict] = {}
    all_records: list[list[int]] = []

    for kind, records in records_by_kind.items():
        all_records.extend(records)
        kind_stmts = sum(len(r) for r in records)
        kind_impl_stmts = 0
        kind_ready_records = 0
        for rec in records:
            rec_ready = True
            for op in rec:
                ne = native_entries.get(op)
                if op < 2768:
                    is_done = op in impl_native or (ne is not None and ne["dispatch"] in ("noop_true", "execute_wse_fallback"))
                else:
                    is_done = op in impl_wse
                if is_done:
                    kind_impl_stmts += 1
                else:
                    rec_ready = False
            if rec_ready:
                kind_ready_records += 1
        per_kind_summary[kind] = {
            "records": len(records),
            "statements": kind_stmts,
            "implemented_or_noop_statements": kind_impl_stmts,
            "ready_without_unimplemented": kind_ready_records,
        }

    for rec in all_records:
        for op in rec:
            call_counts[op] += 1
        for op in set(rec):
            record_presence[op] += 1

    native_unimpl_calls = 0
    native_impl_calls = 0
    native_noop_calls = 0
    native_entity_stub_calls = 0
    wse_impl_calls = 0
    wse_unimpl_calls = 0
    unknown_calls: dict[int, int] = {}

    unimpl_native_rows: list[dict] = []
    unimpl_wse_rows: list[dict] = []
    entity_stub_rows: list[dict] = []

    for op, calls in call_counts.items():
        blocks = record_presence[op]
        if op < 2768:
            ne = native_entries.get(op)
            if ne and ne["dispatch"] in ("noop_true", "execute_wse_fallback"):
                native_noop_calls += calls
            elif op in impl_native:
                native_impl_calls += calls
                if op in ENTITY_ITERATOR_STUBS:
                    native_entity_stub_calls += calls
                    entity_stub_rows.append({
                        "opcode": op,
                        "name": ne["name"] if ne else f"op_{op}",
                        "calls": calls,
                        "records": blocks,
                    })
            else:
                native_unimpl_calls += calls
                unimpl_native_rows.append({
                    "opcode": op,
                    "name": (ne["name"] if ne and ne["name"] else f"unnamed_{op}"),
                    "category": ne["category"] if ne else "unknown",
                    "dispatch": ne["dispatch"] if ne else "unknown",
                    "calls": calls,
                    "records": blocks,
                })
                if ne is None:
                    unknown_calls[op] = calls
        else:
            we = wse_entries.get(op)
            if op in impl_wse:
                wse_impl_calls += calls
            else:
                wse_unimpl_calls += calls
                unimpl_wse_rows.append({
                    "opcode": op,
                    "name": (we["name"] if we and we.get("name") else f"wse_{op}"),
                    "calls": calls,
                    "records": blocks,
                })

    unimpl_native_rows.sort(key=lambda r: (-r["calls"], -r["records"], r["opcode"]))
    unimpl_wse_rows.sort(key=lambda r: (-r["calls"], -r["records"], r["opcode"]))
    entity_stub_rows.sort(key=lambda r: (-r["calls"], r["opcode"]))

    # Cumulative coverage across top K unimplemented native opcodes by call frequency.
    total_unimpl_native_calls = native_unimpl_calls
    cumulative_milestones: list[dict] = []
    ranked_ids = [r["opcode"] for r in unimpl_native_rows]
    for k in (10, 25, 50, 100):
        chosen = set(ranked_ids[:k])
        covered_calls = sum(r["calls"] for r in unimpl_native_rows[:k])
        unlocked_records = 0
        for rec in all_records:
            ok = True
            for op in rec:
                ne = native_entries.get(op)
                if op < 2768:
                    is_done = (
                        op in impl_native
                        or op in chosen
                        or (ne is not None and ne["dispatch"] in ("noop_true", "execute_wse_fallback"))
                    )
                else:
                    is_done = op in impl_wse
                if not is_done:
                    ok = False
                    break
            if ok:
                unlocked_records += 1
        cumulative_milestones.append({
            "top_k": min(k, len(unimpl_native_rows)),
            "covered_unimplemented_calls": covered_calls,
            "share_of_unimplemented_calls_pct": round(
                (100.0 * covered_calls / total_unimpl_native_calls) if total_unimpl_native_calls else 0.0,
                2,
            ),
            "fully_runnable_records": unlocked_records,
        })

    # Category totals for unimplemented native calls.
    cat_calls: Counter[str] = Counter()
    cat_ops: Counter[str] = Counter()
    for r in unimpl_native_rows:
        cat_calls[r["category"]] += r["calls"]
        cat_ops[r["category"]] += 1
    unimpl_categories = [
        {"category": c, "distinct_unimpl_opcodes": cat_ops[c], "calls": cnt}
        for c, cnt in sorted(cat_calls.items(), key=lambda x: (-x[1], x[0]))
    ]

    total_stmts = sum(len(r) for r in all_records)
    return {
        "schema": "wse2-demand-rank/1",
        "sources": per_kind_summary,
        "totals": {
            "records": len(all_records),
            "statements": total_stmts,
            "distinct_opcodes_used": len(call_counts),
            "native_implemented_calls": native_impl_calls,
            "native_entity_iterator_stub_calls": native_entity_stub_calls,
            "native_noop_or_fallback_calls": native_noop_calls,
            "native_unimplemented_calls": native_unimpl_calls,
            "distinct_unimplemented_native_opcodes": len(unimpl_native_rows),
            "wse_implemented_calls": wse_impl_calls,
            "wse_unimplemented_calls": wse_unimpl_calls,
            "distinct_unimplemented_wse_opcodes": len(unimpl_wse_rows),
            "unknown_opcode_calls": unknown_calls,
        },
        "milestones": cumulative_milestones,
        "unimplemented_native_categories": unimpl_categories,
        "top_unimplemented_native": unimpl_native_rows[:top_n],
        "top_unimplemented_wse": unimpl_wse_rows[:top_n],
        "entity_iterator_stubs": entity_stub_rows,
    }


def render_markdown(res: dict) -> str:
    t = res["totals"]
    lines = [
        "# Demand Ranking Report (Counts Only)",
        "",
        "## Input summary",
        "",
        "| File kind | Records | Statements | Already covered statements | Zero-unimplemented records |",
        "| --- | ---: | ---: | ---: | ---: |",
    ]
    for kind, s in res["sources"].items():
        lines.append(
            f"| `{kind}` | {s['records']} | {s['statements']} | {s['implemented_or_noop_statements']} | {s['ready_without_unimplemented']} |"
        )
    lines.append(
        f"| **Total** | **{t['records']}** | **{t['statements']}** | "
        f"**{t['native_implemented_calls'] + t['native_noop_or_fallback_calls'] + t['wse_implemented_calls']}** | — |"
    )

    lines.extend([
        "",
        "## Aggregate breakdown",
        "",
        "| Metric | Value |",
        "| --- | ---: |",
        f"| Distinct opcodes used | {t['distinct_opcodes_used']} |",
        f"| Native implemented calls (`block_vm.h` + `native_*.h`) | {t['native_implemented_calls']} |",
        *(
            [
                f"| Native entity-iterator zero-stub calls ({', '.join(str(x) for x in sorted(ENTITY_ITERATOR_STUBS))}) | {t['native_entity_iterator_stub_calls']} |"
            ]
            if ENTITY_ITERATOR_STUBS
            else []
        ),
        f"| Native `noop_true` / fallback calls | {t['native_noop_or_fallback_calls']} |",
        f"| Native unimplemented calls | {t['native_unimplemented_calls']} ({t['distinct_unimplemented_native_opcodes']} distinct opcodes) |",
        f"| WSE implemented calls | {t['wse_implemented_calls']} |",
        f"| WSE unimplemented calls | {t['wse_unimplemented_calls']} ({t['distinct_unimplemented_wse_opcodes']} distinct opcodes) |",
        "",
        "## Cumulative call share if top K native opcodes are implemented",
        "",
        "| Top K unimplemented native | Unimplemented calls covered | Share of missing native calls | Fully covered records |",
        "| ---: | ---: | ---: | ---: |",
    ])
    for m in res["milestones"]:
        lines.append(
            f"| {m['top_k']} | {m['covered_unimplemented_calls']} | {m['share_of_unimplemented_calls_pct']}% | {m['fully_runnable_records']} |"
        )

    lines.extend([
        "",
        "## Top unimplemented native opcodes",
        "",
        "| Rank | Opcode | Name | Category | Calls | Records |",
        "| ---: | ---: | --- | --- | ---: | ---: |",
    ])
    for i, r in enumerate(res["top_unimplemented_native"], 1):
        lines.append(
            f"| {i} | {r['opcode']} | `{r['name']}` | `{r['category']}` | {r['calls']} | {r['records']} |"
        )

    if res["entity_iterator_stubs"]:
        lines.extend([
            "",
            "## Entity-iterator zero-iteration stubs used",
            "",
            "| Opcode | Name | Calls | Records |",
            "| ---: | --- | ---: | ---: |",
        ])
        for r in res["entity_iterator_stubs"]:
            lines.append(f"| {r['opcode']} | `{r['name']}` | {r['calls']} | {r['records']} |")

    if res["top_unimplemented_wse"]:
        lines.extend([
            "",
            "## Top unimplemented WSE opcodes used",
            "",
            "| Rank | Opcode | Name | Calls | Records |",
            "| ---: | ---: | --- | ---: | ---: |",
        ])
        for i, r in enumerate(res["top_unimplemented_wse"], 1):
            lines.append(f"| {i} | {r['opcode']} | `{r['name']}` | {r['calls']} | {r['records']} |")

    lines.extend([
        "",
        "## Unimplemented native demand by category",
        "",
        "| Category | Distinct opcodes | Calls |",
        "| --- | ---: | ---: |",
    ])
    for c in res["unimplemented_native_categories"]:
        lines.append(f"| `{c['category']}` | {c['distinct_unimpl_opcodes']} | {c['calls']} |")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    ap = argparse.ArgumentParser(description="Rank unimplemented opcodes by module demand (counts only)")
    ap.add_argument("--module-dir", type=Path, help="Directory containing scripts.txt / triggers.txt / simple_triggers.txt")
    ap.add_argument("--scripts", action="append", default=[], type=Path, help="Path to scripts.txt")
    ap.add_argument("--triggers", action="append", default=[], type=Path, help="Path to triggers.txt")
    ap.add_argument("--simple-triggers", action="append", default=[], type=Path, help="Path to simple_triggers.txt")
    ap.add_argument("--native-map", type=Path, default=repo_root / "docs" / "native_opcodes.json")
    ap.add_argument("--wse-map", type=Path, default=repo_root / "docs" / "wse_opcodes.json")
    ap.add_argument("--block-vm", type=Path, default=repo_root / "engine" / "include" / "wse" / "vm" / "block_vm.h")
    ap.add_argument("--wse-header", type=Path, default=repo_root / "engine" / "include" / "wse" / "vm" / "wse_opcodes.h")
    ap.add_argument("--execute-wse", type=Path, default=repo_root / "engine" / "include" / "wse" / "vm")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--json", type=Path, help="Optional output JSON path")
    ap.add_argument("--md", type=Path, help="Optional output Markdown path")
    args = ap.parse_args()

    scripts_paths = list(args.scripts)
    triggers_paths = list(args.triggers)
    simple_paths = list(args.simple_triggers)
    if args.module_dir:
        for fname, dest in (
            ("scripts.txt", scripts_paths),
            ("triggers.txt", triggers_paths),
            ("simple_triggers.txt", simple_paths),
        ):
            cand = args.module_dir / fname
            if cand.is_file():
                dest.append(cand)

    records_by_kind: dict[str, list[list[int]]] = defaultdict(list)
    for p in scripts_paths:
        records_by_kind["scripts.txt"].extend(parse_scripts_file(p))
    for p in triggers_paths:
        records_by_kind["triggers.txt"].extend(parse_triggers_file(p))
    for p in simple_paths:
        records_by_kind["simple_triggers.txt"].extend(parse_simple_triggers_file(p))

    if not records_by_kind:
        raise SystemExit("No module files provided (pass --module-dir or --scripts/--triggers/--simple-triggers)")

    result = analyze_demand(
        records_by_kind=dict(records_by_kind),
        native_map_path=args.native_map,
        wse_map_path=args.wse_map,
        block_vm_path=args.block_vm,
        wse_header_path=args.wse_header,
        execute_wse_path=args.execute_wse,
        top_n=args.top,
    )

    md = render_markdown(result)
    if args.md:
        args.md.parent.mkdir(parents=True, exist_ok=True)
        args.md.write_text(md, encoding="utf-8")
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    print(md)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
