#!/usr/bin/env python3
"""Extract the v1.1.5.5 native Warband opcode map from mb_warband_wse2_x64.exe.

Combines:
- mbStatementBlock control-flow dispatch (< 30) verified in mb_warband_wse2_x64.exe
- mbStatement::execute native jump table at 0x1401e22f0 (opcodes 30..2767)
- Public Warband SDK constants/comments from header_operations.py
- Implemented opcode scan from engine/include/wse/vm/block_vm.h
- Community names (COMMUNITY_NAMES) for live handlers the SDK header does not define
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


IMAGE_BASE = 0x140000000
SWITCH_GUARD_VA = 0x1401C63E0
SWITCH_GUARD_EXPECTED = bytes.fromhex("83c6e281feb10a00000f874fbe0100")
JUMP_TABLE_VA = 0x1401E22F0
NATIVE_MIN = 30
NATIVE_MAX = 2767
NATIVE_SLOTS = NATIVE_MAX - NATIVE_MIN + 1  # 2738
FALLBACK_WSE_VA = 0x1401E223E
NOOP_TRUE_VA = 0x1401E226B

# Control-flow operations (< 30) dispatched inside mbStatementBlock
# (analyze 0x1401f6740, execute 0x1401f69d0, startLoop 0x1401f7290,
# endLoop 0x1401f7910, canLoop 0x1401f7f00).
CONTROL_FLOW_VAS: dict[int, tuple[str, str]] = {
    1: ("0x1401f6d4c", "mbStatementBlock::execute"),
    3: ("0x1401f6e8c", "mbStatementBlock::execute"),
    4: ("0x1401f6ebb", "mbStatementBlock::execute"),
    5: ("0x1401f6efd", "mbStatementBlock::execute"),
    6: ("0x1401f72f1", "mbStatementBlock::startLoop"),
    7: ("0x1401f730f", "mbStatementBlock::startLoop"),
    8: ("0x1401f6d5e", "mbStatementBlock::execute"),
    9: ("0x1401f6e1b", "mbStatementBlock::execute"),
    11: ("0x1401f7516", "mbStatementBlock::startLoop"),
    12: ("0x1401f732e", "mbStatementBlock::startLoop"),
    16: ("0x1401f759c", "mbStatementBlock::startLoop"),
    17: ("0x1401f7531", "mbStatementBlock::startLoop"),
    18: ("0x1401f7692", "mbStatementBlock::startLoop"),
    21: ("0x1401f7176", "mbStatementBlock::execute"),
    22: ("0x1401f71ba", "mbStatementBlock::execute"),
    23: ("0x1401f71ee", "mbStatementBlock::execute"),
}

ENTITY_ITERATOR_STUBS: set[int] = set()

# Names for opcodes that have no constant in the public header_operations.py.
# Source: the community operations reference at COMMUNITY_SOURCE_URL, cross-checked on
# 2026-11-14 against this map: all 1,059 native names it shares with the SDK header carry
# the same opcode number, and its three extra names land exactly on the three live
# handlers the SDK header leaves unnamed. The names are community-assigned, not retail
# symbols; the handler VAs remain the only binary evidence. An SDK constant always wins.
COMMUNITY_SOURCE_URL = "https://mbcommands.fandom.com/wiki/Operations"
COMMUNITY_NAMES: dict[int, dict] = {
    53: {
        "name": "is_cheat_mode_enabled",
        "signature": "(is_cheat_mode_enabled),",
        "can_fail": True,
        "is_lhs": False,
    },
    2180: {
        "name": "store_main_party_wounded",
        "signature": "(store_main_party_wounded, <destination>),",
        "can_fail": False,
        "is_lhs": True,
    },
    2749: {
        "name": "str_store_agent_face_keys",
        "signature": "(str_store_agent_face_keys, <string_reg>, <agent_id>),",
        "can_fail": False,
        "is_lhs": False,
    },
}

# Community-documented opcodes that have no handler at all in WSE2 v1.1.5.5. Recorded so a
# module that still compiles one in is recognised by name instead of as an unknown ID.
COMMUNITY_DEAD_NAMES: dict[int, dict] = {
    15: {
        "name": "try_for_attached_parties",
        "signature": "(try_for_attached_parties, <attached_party_no>, <root_party_no>),",
        "note": "Deprecated vanilla loop; no mbStatementBlock handler in WSE2 v1.1.5.5",
    },
}


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_sdk_operations(sdk_path: Path) -> dict:
    """Parse header_operations.py constants, aliases, comments, and attribute lists."""
    text = sdk_path.read_text(encoding="utf-8", errors="replace")
    ns: dict[str, object] = {}
    exec(compile(text, str(sdk_path), "exec"), ns)

    lines = text.splitlines()
    # Map identifier -> (line_no, raw_comment)
    by_id: dict[int, list[dict]] = defaultdict(list)
    modifiers: list[dict] = []
    assign_re = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^#\r\n]+?)(?:\s*#\s*(.*))?$")

    for line_no, raw in enumerate(lines, 1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        m = assign_re.match(stripped)
        if not m:
            continue
        name, _expr, comment = m.group(1), m.group(2).strip(), (m.group(3) or "").strip()
        val = ns.get(name)
        if not isinstance(val, int) or isinstance(val, bool):
            continue
        if val >= 0x40000000:
            modifiers.append({
                "name": name,
                "value": val,
                "hex": hex(val),
                "sdk_line": line_no,
                "sdk_comment": comment,
            })
        elif 0 <= val < 2800:
            if not any(d["name"] == name for d in by_id[val]):
                by_id[val].append({
                    "name": name,
                    "sdk_line": line_no,
                    "sdk_comment": comment,
                })

    def id_set(var_name: str) -> set[int]:
        seq = ns.get(var_name, [])
        if not isinstance(seq, (list, tuple, set)):
            return set()
        return {int(x) & 0x0FFFFFFF for x in seq if isinstance(x, int)}

    return {
        "by_id": dict(by_id),
        "modifiers": modifiers,
        "lhs": id_set("lhs_operations"),
        "global_lhs": id_set("global_lhs_operations"),
        "can_fail": id_set("can_fail_operations"),
    }


def scan_implemented_from_block_vm(block_vm_path: Path) -> tuple[set[int], set[int]]:
    """Scan block_vm.h and sibling native_*.h for implemented control-flow (<30) and switch (30..2767) opcodes."""
    paths = [block_vm_path] + sorted(block_vm_path.parent.glob("native_*.h"))

    texts = [p.read_text(encoding="utf-8", errors="replace") for p in paths]
    core_map: dict[str, int] = {}
    after_core_chunks: list[str] = []
    for text in texts:
        last_end = 0
        for core_block_m in re.finditer(
            r"namespace\s+core_op\s*\{(.*?)\}\s*//\s*namespace\s+core_op", text, re.S
        ):
            for m in re.finditer(
                r"k([A-Za-z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)U?", core_block_m.group(1)
            ):
                core_map["k" + m.group(1)] = int(m.group(2), 0)
            last_end = core_block_m.end()
        if last_end:
            after_core_chunks.append(text[last_end:])

    if not core_map:
        raise SystemExit(f"Could not find namespace core_op in {block_vm_path}")

    disp_pattern = re.compile(
        r"dispatch_native(?:_[a-z0-9_]+)?_statement(?:_ext)?\s*\([^)]*\)\s*\{(.*?)\n\s*(?:\+\+ctx\.unimplementedCount;|return\s+(?:NativeDispatch::unimplemented|dispatch_native(?:_[a-z0-9_]+)?_statement(?:_ext)?\([^;]*\));)",
        re.S,
    )
    impl_switch: set[int] = set()
    found_dispatch = False
    for text in texts:
        for disp_m in disp_pattern.finditer(text):
            found_dispatch = True
            disp_body = disp_m.group(1)
            for m in re.finditer(r"case\s+core_op::(k[A-Za-z0-9_]+)\s*:", disp_body):
                op = core_map.get(m.group(1))
                if op is not None and NATIVE_MIN <= op <= NATIVE_MAX:
                    impl_switch.add(op)
            for m in re.finditer(r"case\s+(0x[0-9A-Fa-f]+|\d+)U?\s*:", disp_body):
                op = int(m.group(1), 0)
                if NATIVE_MIN <= op <= NATIVE_MAX:
                    impl_switch.add(op)

    if not found_dispatch:
        raise SystemExit(f"Could not locate dispatch_native_statement in {block_vm_path}")

    # Control-flow ops (<30): check references outside namespace core_op and outside entity stub comment.
    after_core = "\n".join(after_core_chunks)
    impl_cf: set[int] = set()
    for name, op in core_map.items():
        if op < NATIVE_MIN and re.search(rf"\bcore_op::{re.escape(name)}\b", after_core):
            impl_cf.add(op)

    return impl_cf, impl_switch


def classify_category(opcode: int, name: str | None) -> str:
    if opcode < NATIVE_MIN:
        return "control_flow"
    if not name:
        return "undocumented"
    if name in ("ge", "eq", "gt", "is_between"):
        return "comparisons_and_branching"
    if name.startswith("val_") or (2100 <= opcode <= 2149):
        return "arithmetic_registers_and_vars"
    if "_slot_" in name or "_get_slot" in name or "_set_slot" in name or "store_script_param" in name:
        return "entity_slots"
    if name.startswith("str_") or opcode in (2318, 2319):
        return "strings"
    if name.startswith(("try_for_", "call_script")):
        return "control_flow"
    if name.startswith(("position_", "map_get_", "init_position", "copy_position", "get_angle", "get_distance", "set_fixed_point", "convert_")):
        return "positions_and_math"
    if name.startswith(("agent_", "deliver_damage", "set_spawn_position", "spawn_agent", "spawn_horse", "remove_agent")):
        return "agents"
    if name.startswith(("party_", "spawn_around_party", "heal_party", "distribute_party", "inflict_casualties")):
        return "parties"
    if name.startswith(("troop_", "add_xp", "add_gold", "remove_gold")):
        return "troops"
    if "party" in name:
        return "parties"
    if "troop" in name:
        return "troops"
    if name.startswith(("scene_prop_", "prop_instance_", "replace_prop", "scene_", "mission_cam", "set_rain", "set_fog", "entry_point", "spawn_scene_prop", "modify_visitors", "reset_visitors", "add_visitors", "set_visitor", "set_jump_entry", "set_jump_mission", "finish_mission", "get_scene_boundaries", "set_shader_param")):
        return "scenes_props_and_mission"
    if name.startswith(("player_", "multiplayer_", "server_", "get_max_players", "get_my_player")):
        return "multiplayer_and_players"
    if name.startswith(("presentation_", "create_", "overlay_", "set_container_", "mouse_get", "is_presentation", "start_presentation", "start_background_presentation", "change_screen_", "tutorial_", "dialog_box", "question_box", "display_")):
        return "ui_presentations_and_menus"
    if name.startswith(("item_", "cur_item_", "store_item_")):
        return "items"
    if name.startswith(("faction_", "quest_", "menu_", "option_")):
        return "factions_quests_menus_options"
    if name.startswith(("play_", "stop_all_sounds", "music_", "particle_system_", "add_point_light", "set_light_", "cur_tableau_", "tableau_material_")):
        return "audio_particles_and_tableau"
    if name.startswith(("game_key_", "key_", "omit_key_")):
        return "input"
    if name.startswith("face_keys_") or "face_keys" in name:
        return "face_keys"
    if name.startswith(("store_", "assign", "shuffle_range", "set_global", "get_global", "set_show", "jump_to", "rest_for", "set_player", "set_camera", "is_camera", "get_operation", "set_physics", "set_result", "is_vanilla", "race_completed", "set_merchandise", "encounter_attack")):
        return "game_state_and_stores"
    if name.startswith(("team_", "class_", "spawn_item", "cast_ray", "is_currently_night")):
        return "mission_combat_and_teams"
    return "other_native"


def compress_ranges(nums: list[int]) -> list[list[int]]:
    if not nums:
        return []
    out: list[list[int]] = []
    start = prev = nums[0]
    for n in nums[1:]:
        if n == prev + 1:
            prev = n
        else:
            out.append([start, prev])
            start = prev = n
    out.append([start, prev])
    return out


def extract_map(exe_path: Path, sdk_path: Path, block_vm_path: Path) -> dict:
    import pefile  # imported here so the renderer works without the PE dependency

    sha = sha256_file(exe_path)
    pe = pefile.PE(str(exe_path), fast_load=False)
    base = pe.OPTIONAL_HEADER.ImageBase
    if base != IMAGE_BASE:
        raise SystemExit(f"Unexpected ImageBase {hex(base)} (expected {hex(IMAGE_BASE)})")

    text_sec = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"), None)
    if text_sec is None:
        raise SystemExit("No .text section found")
    text_lo = base + text_sec.VirtualAddress
    text_hi = text_lo + text_sec.Misc_VirtualSize

    guard = pe.get_data(SWITCH_GUARD_VA - base, len(SWITCH_GUARD_EXPECTED))
    if guard != SWITCH_GUARD_EXPECTED:
        raise SystemExit(
            f"Switch guard mismatch at {hex(SWITCH_GUARD_VA)}: "
            f"got {guard.hex()}, expected {SWITCH_GUARD_EXPECTED.hex()}"
        )

    rvas = pe.get_data(JUMP_TABLE_VA - base, NATIVE_SLOTS * 4)
    import struct
    slots = struct.unpack(f"<{NATIVE_SLOTS}I", rvas)

    sdk = parse_sdk_operations(sdk_path)
    by_id: dict[int, list[dict]] = sdk["by_id"]
    impl_cf, impl_switch = scan_implemented_from_block_vm(block_vm_path)

    entries: list[dict] = []
    unused_control_flow_ids: list[int] = []
    unused_fallback_ids: list[int] = []
    dispatch_counts: Counter[str] = Counter()

    # 1. Control-flow slots 0..29
    for op in range(NATIVE_MIN):
        sdk_defs = by_id.get(op, [])
        if op not in CONTROL_FLOW_VAS:
            unused_control_flow_ids.append(op)
            if sdk_defs:
                raise SystemExit(f"Unexpected SDK opcode < 30 without control-flow handler: {op} {sdk_defs}")
            continue
        va_str, func_name = CONTROL_FLOW_VAS[op]
        names = [d["name"] for d in sdk_defs]
        primary = names[0] if names else None
        aliases = names[1:]
        implemented = op in impl_cf
        status = (
            "entity_iterator_zero_stub"
            if op in ENTITY_ITERATOR_STUBS
            else ("implemented" if implemented else "unimplemented")
        )
        entries.append({
            "opcode": op,
            "hex": hex(op),
            "range": "control_flow",
            "dispatch": "statement_block",
            "handler_va": va_str,
            "dispatch_function": func_name,
            "name": primary,
            "aliases": aliases,
            "category": classify_category(op, primary),
            "can_fail": op in sdk["can_fail"],
            "is_lhs": op in sdk["lhs"],
            "is_global_lhs": op in sdk["global_lhs"],
            "implemented": implemented,
            "status": status,
            "sdk_line": sdk_defs[0]["sdk_line"] if sdk_defs else None,
            "sdk_comment": sdk_defs[0]["sdk_comment"] if sdk_defs else "",
        })
        dispatch_counts["statement_block"] += 1

    # 2. Switch-table slots 30..2767
    unnamed_live: list[int] = []
    community_named: list[dict] = []
    noop_list: list[dict] = []
    fallback_named: list[dict] = []

    for idx, rva in enumerate(slots):
        op = NATIVE_MIN + idx
        va = base + rva
        if not (text_lo <= va < text_hi):
            raise SystemExit(f"Jump-table slot {op} points outside .text: {hex(va)}")

        sdk_defs = by_id.get(op, [])
        names = [d["name"] for d in sdk_defs]
        community = None if sdk_defs else COMMUNITY_NAMES.get(op)
        primary = names[0] if names else (community["name"] if community else None)
        aliases = names[1:]
        if community and va in (FALLBACK_WSE_VA, NOOP_TRUE_VA):
            raise SystemExit(
                f"Community name {community['name']} ({op}) does not map to a live handler: {hex(va)}"
            )

        if va == FALLBACK_WSE_VA:
            dispatch = "execute_wse_fallback"
            dispatch_counts[dispatch] += 1
            if not sdk_defs:
                unused_fallback_ids.append(op)
                continue
            fallback_named.append({"opcode": op, "name": primary, "aliases": aliases})
        elif va == NOOP_TRUE_VA:
            dispatch = "noop_true"
            dispatch_counts[dispatch] += 1
            noop_list.append({"opcode": op, "name": primary, "aliases": aliases})
        else:
            dispatch = "switch_handler"
            dispatch_counts[dispatch] += 1
            if not sdk_defs:
                unnamed_live.append(op)
                if community:
                    community_named.append({
                        "opcode": op,
                        "name": community["name"],
                        "handler_va": hex(va),
                        "signature": community["signature"],
                    })

        implemented = op in impl_switch
        if dispatch == "execute_wse_fallback":
            status = "fallback_returns_false"
        elif dispatch == "noop_true":
            status = "noop_in_wse2"
        else:
            status = "implemented" if implemented else "unimplemented"

        entries.append({
            "opcode": op,
            "hex": hex(op),
            "range": "native_switch",
            "dispatch": dispatch,
            "handler_va": hex(va),
            "dispatch_function": "mbStatement::execute",
            "name": primary,
            "aliases": aliases,
            "category": classify_category(op, primary),
            "can_fail": op in sdk["can_fail"] or bool(community and community["can_fail"]),
            "is_lhs": op in sdk["lhs"] or bool(community and community["is_lhs"]),
            "is_global_lhs": op in sdk["global_lhs"],
            "implemented": implemented,
            "status": status,
            "sdk_line": sdk_defs[0]["sdk_line"] if sdk_defs else None,
            "sdk_comment": sdk_defs[0]["sdk_comment"] if sdk_defs else "",
        })
        if community:
            entries[-1]["name_source"] = "community_wiki"
            entries[-1]["community_signature"] = community["signature"]

    community_dead: list[dict] = []
    for op, dead in sorted(COMMUNITY_DEAD_NAMES.items()):
        if op in by_id or op not in unused_control_flow_ids + unused_fallback_ids:
            raise SystemExit(f"Community dead name {dead['name']} ({op}) is live or SDK-defined")
        community_dead.append({"opcode": op, **dead})

    # Verify all SDK IDs in 0..2767 were captured.
    captured_ids = {e["opcode"] for e in entries}
    missing_sdk = sorted(op for op in by_id if op not in captured_ids)
    if missing_sdk:
        raise SystemExit(f"SDK IDs missing from native map: {missing_sdk}")

    # Category rollup over live/named entries.
    cat_totals: Counter[str] = Counter()
    cat_impl: Counter[str] = Counter()
    for e in entries:
        if e["dispatch"] in ("statement_block", "switch_handler"):
            cat_totals[e["category"]] += 1
            if e["implemented"]:
                cat_impl[e["category"]] += 1

    categories = [
        {"category": cat, "live_handlers": cnt, "implemented": cat_impl[cat]}
        for cat, cnt in sorted(cat_totals.items(), key=lambda x: (-x[1], x[0]))
    ]

    summary = {
        "control_flow_slots": NATIVE_MIN,
        "control_flow_live": dispatch_counts["statement_block"],
        "control_flow_unused": len(unused_control_flow_ids),
        "switch_total_slots": NATIVE_SLOTS,
        "switch_handlers": dispatch_counts["switch_handler"],
        "noop_true": dispatch_counts["noop_true"],
        "execute_wse_fallback_total": dispatch_counts["execute_wse_fallback"],
        "execute_wse_fallback_named": len(fallback_named),
        "execute_wse_fallback_unused": len(unused_fallback_ids),
        "total_live_native_handlers": dispatch_counts["statement_block"] + dispatch_counts["switch_handler"],
        "sdk_unique_ids_0_2767": len(by_id),
        "sdk_total_symbols_0_2767": sum(len(v) for v in by_id.values()),
        "unnamed_live_handlers": unnamed_live,
        "community_named_handlers": community_named,
        "community_named_unused_ids": community_dead,
        "noop_true_slots": noop_list,
        "fallback_named_slots": fallback_named,
        "implemented_control_flow": len(impl_cf),
        "entity_iterator_stubs": sorted(ENTITY_ITERATOR_STUBS),
        "implemented_switch": len(impl_switch),
        "implemented_total": len(impl_cf) + len(impl_switch),
    }

    return {
        "schema": "wse2-native-opcode-map/1",
        "target": exe_path.name,
        "release": "v1.1.5.5",
        "exe_sha256": sha,
        "community_name_source": COMMUNITY_SOURCE_URL,
        "function": {
            "symbol": "?execute@mbStatement@@QEAA_NH@Z",
            "switch_guard_va": hex(SWITCH_GUARD_VA),
            "jump_table_va": hex(JUMP_TABLE_VA),
            "fallback_va": hex(FALLBACK_WSE_VA),
            "noop_va": hex(NOOP_TRUE_VA),
            "control_flow_range": [0, NATIVE_MIN - 1],
            "switch_range": [NATIVE_MIN, NATIVE_MAX],
            "opcode_mask": "0x0fffffff",
        },
        "summary": summary,
        "categories": categories,
        "sdk_modifiers_and_aliases": sdk["modifiers"],
        "entries": entries,
        "unused_control_flow_ids": unused_control_flow_ids,
        "unused_fallback_ranges": compress_ranges(unused_fallback_ids),
    }


def render_markdown(doc: dict) -> str:
    s = doc["summary"]
    lines = [
        "# WSE2 Native Opcode Map (`0–2767`)",
        "",
        "Generated by `tools/extract_native_opcodes.py` from:",
        "",
        f"- `mbStatement::execute` switch guard `{doc['function']['switch_guard_va']}` and jump table `{doc['function']['jump_table_va']}` (`{doc['target']}`, `{doc['release']}`, SHA-256 `{doc['exe_sha256']}`)",
        "- `mbStatementBlock::{analyze, execute, startLoop, endLoop, canLoop}` control-flow handlers (`0–29`)",
        "- Public Warband SDK constants (`header_operations.py`) and implemented scan of `engine/include/wse/vm/block_vm.h` + `native_*.h`",
        "",
        "Machine-readable copy: `docs/native_opcodes.json`.",
        "",
        "## Dispatch summary",
        "",
        "| Metric | Value | Notes |",
        "| --- | ---: | --- |",
        f"| Control-flow range (`0–29`) live handlers | {s['control_flow_live']} | {s['control_flow_unused']} unused IDs |",
        f"| Native switch table (`30–2767`) total slots | {s['switch_total_slots']} | Indexed at `{doc['function']['jump_table_va']}` |",
        f"| Live switch handlers (`switch_handler`) | {s['switch_handlers']} | Unique `.text` handlers (incl. {len(s['unnamed_live_handlers'])} without an SDK constant) |",
        f"| Shared no-op `return true` stubs (`noop_true`) | {s['noop_true']} | Jump directly to `{doc['function']['noop_va']}` |",
        f"| Fallback to `executeWSE` (`execute_wse_fallback`) | {s['execute_wse_fallback_total']} | Jump to `{doc['function']['fallback_va']}`; {s['execute_wse_fallback_named']} SDK-named (`1004`), {s['execute_wse_fallback_unused']} empty holes |",
        f"| **Total live native handlers (`0–2767`)** | **{s['total_live_native_handlers']}** | {s['control_flow_live']} control-flow + {s['switch_handlers']} switch |",
        f"| SDK unique IDs in `0–2767` | {s['sdk_unique_ids_0_2767']} | {s['sdk_total_symbols_0_2767']} symbol definitions incl. aliases |",
        f"| Implemented in VM (`block_vm.h` + `native_*.h`) (`control_flow` / `switch`) | {s['implemented_control_flow']} / {s['implemented_switch']} | Total implemented: **{s['implemented_total']}**"
        + (
            f" ({len(s['entity_iterator_stubs'])} entity iterators run 0 iterations until world containers load)"
            if s["entity_iterator_stubs"]
            else ""
        )
        + " |",
        "",
        "## Special slots",
        "",
        "### Live native handlers without a public SDK constant",
        "",
        f"Names and signatures come from the community reference <{doc['community_name_source']}>;",
        "they are not retail symbols. The handler VA is the binary evidence.",
        "",
        "| Opcode | Hex | Handler VA | Community name | Community signature |",
        "| ---: | --- | --- | --- | --- |",
    ]
    entry_by_id = {e["opcode"]: e for e in doc["entries"]}
    for op in s["unnamed_live_handlers"]:
        e = entry_by_id[op]
        name_cell = f"`{e['name']}`" if e["name"] else "*(unnamed)*"
        sig_cell = f"`{e['community_signature']}`" if e.get("community_signature") else ""
        lines.append(f"| {op} | `{e['hex']}` | `{e['handler_va']}` | {name_cell} | {sig_cell} |")

    lines.extend([
        "",
        "### Community-named opcodes with no handler in WSE2",
        "",
        "| Opcode | Hex | Community name | Notes |",
        "| ---: | --- | --- | --- |",
    ])
    for item in s["community_named_unused_ids"]:
        lines.append(f"| {item['opcode']} | `{hex(item['opcode'])}` | `{item['name']}` | {item['note']} |")

    lines.extend([
        "",
        "### Shared `noop_true` slots (`0x1401e226b`)",
        "",
        "| Opcode | Hex | SDK Name |",
        "| ---: | --- | --- |",
    ])
    for item in s["noop_true_slots"]:
        lines.append(f"| {item['opcode']} | `{hex(item['opcode'])}` | `{item['name']}` |")

    lines.extend([
        "",
        "### SDK-named slots routed to `executeWSE` fallback (`0x1401e223e`)",
        "",
        "| Opcode | Hex | SDK Name(s) | Behavior in WSE2 |",
        "| ---: | --- | --- | --- |",
    ])
    for item in s["fallback_named_slots"]:
        all_names = ", ".join([item["name"], *item["aliases"]])
        lines.append(f"| {item['opcode']} | `{hex(item['opcode'])}` | `{all_names}` | `executeWSE` returns `false` for IDs `< 2800` (`is_vanilla_warband` fails under WSE2) |")

    lines.extend([
        "",
        "## Category breakdown (live handlers)",
        "",
        "| Category | Live handlers | Implemented (`block_vm.h` + `native_*.h`) |",
        "| --- | ---: | ---: |",
    ])
    for cat in doc["categories"]:
        lines.append(f"| `{cat['category']}` | {cat['live_handlers']} | {cat['implemented']} |")

    lines.extend([
        "",
        "## Mapped & Live Operations (`0–2767`)",
        "",
        "| Opcode | Hex | SDK Name | Dispatch | Handler VA | Category | Status |",
        "| ---: | --- | --- | --- | --- | --- | --- |",
    ])
    for e in doc["entries"]:
        name_cell = f"`{e['name']}`" if e["name"] else "*(unnamed)*"
        if e.get("name_source") == "community_wiki":
            name_cell += " *(community name)*"
        if e["aliases"]:
            name_cell += " (" + ", ".join(f"`{a}`" for a in e["aliases"]) + ")"
        lines.append(
            f"| {e['opcode']} | `{e['hex']}` | {name_cell} | `{e['dispatch']}` | `{e['handler_va']}` | `{e['category']}` | `{e['status']}` |"
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract native v1.1.5.5 opcode map from mb_warband_wse2_x64.exe")
    ap.add_argument("--exe", required=True, type=Path, help="Path to v1.1.5.5 mb_warband_wse2_x64.exe")
    ap.add_argument("--sdk", required=True, type=Path, help="Path to Warband header_operations.py")
    ap.add_argument(
        "--block-vm",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "engine" / "include" / "wse" / "vm" / "block_vm.h",
        help="Path to engine/include/wse/vm/block_vm.h",
    )
    ap.add_argument(
        "--out-json",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "docs" / "native_opcodes.json",
    )
    ap.add_argument(
        "--out-md",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "docs" / "NATIVE_OPCODES.md",
    )
    args = ap.parse_args()

    doc = extract_map(args.exe, args.sdk, args.block_vm)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(doc, indent=1) + "\n", encoding="utf-8")
    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.write_text(render_markdown(doc), encoding="utf-8")

    s = doc["summary"]
    print(
        f"wrote {args.out_json} and {args.out_md}: "
        f"{s['control_flow_live']} control-flow + {s['switch_handlers']} switch = "
        f"{s['total_live_native_handlers']} live handlers "
        f"({s['noop_true']} noop_true, {s['execute_wse_fallback_total']} fallback), "
        f"{s['implemented_total']} implemented in block_vm.h"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
