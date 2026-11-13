#!/usr/bin/env python3
"""Build the curated WSE2 symbol map from PDB-derived artifacts.

Inputs are generated outside the repository (retail PDB data is never committed):

  layouts/<Class>.json   from: python tools/pdb_symbols.py <pdb> --layout <Class> --json ...
  globals_x64.json       from: python tools/pdb_symbols.py <pdb> --globals --exe <exe> --json ...
  vtables_x64.json       from: python tools/vtables.py

Usage:
  python tools/build_symbol_map.py --layouts <dir> --globals <json> --vtables <json> \
      --out docs/SYMBOL_MAP.json
"""

import argparse
import json
import os
import re

CLASSES = [
    "rglString",
    "rglStream",
    "mbStatementBlock",
    "mbStatement",
    "mbLoop",
    "mbLoopManager",
    "mbCallStack",
    "mbScript",
    "mbScriptManager",
    "mbTrigger",
    "mbTriggerManager",
    "mbSimpleTrigger",
    "mbSimpleTriggerManager",
    "WSELuaManager",
    "mbBasicGame",
    "mbGame",
    "mbMission",
    "mbAgent",
    "mbTroop",
    "mbItemKind",
    "mbItemKindVariation",
    "mbSkill",
    "mbParty",
    "mbPartyTemplate",
    "mbFaction",
    "mbQuest",
    "mbSite",
    "mbMissionObject",
    "mbnetPlayer",
    "mbSceneProp",
    "rglMesh",
    "rglMetaMesh",
    "rglRenderer",
    "mbMissionTemplate",
    "gameOperation",
    "gameConstTable",
    "rglAppTimer",
    "rglTimer",
    "rglTextStream",
    "rglBinaryStream",
    "rglInputTextFileStream",
    "rglOutputTextFileStream",
    "mbSlots",
    "rglVector4",
    "rglTransform",
    "rglMersenneTwister",
    "rglSectionlessIni",
    "WSEObjectManager",
    "WSEObjectContainer",
    "WSEObjectType",
    "WSEArraySortOptions",
    "WSEBasicDynMultiArray",
    "WSEDynMultiArray<int>",
    "WSEDynMultiArray<rglTransform>",
    "WSEDynMultiArray<std::basic_string<char,std::char_traits<char>,std::allocator<char> > >",
    "mu::Parser",
    "mu::ParserBase",
    "WSEProfiling",
]

LAYOUT_FILES = {
    "WSEDynMultiArray<int>": "WSEDynMultiArray_int.json",
    "WSEDynMultiArray<rglTransform>": "WSEDynMultiArray_rglTransform.json",
    "WSEDynMultiArray<std::basic_string<char,std::char_traits<char>,std::allocator<char> > >": "WSEDynMultiArray_string.json",
    "mu::Parser": "mu_Parser.json",
    "mu::ParserBase": "mu_ParserBase.json",
}

SYMBOL_TOKENS = [
    "Parser@mu@@",
    "ParserBase@mu@@",
    "WSEDynMultiArray",
]

GLOBAL_NAMES = {
    "g_scriptManager",
    "g_luaManager",
    "g_game",
    "g_mission",
    "g_missionTemplates",
    "g_itemKinds",
    "g_partyTemplates",
    "g_sceneProps",
    "g_renderer",
    "g_gameScreen",
}

# .rdata string literals located with tools/pe_evidence.py-style scans.
STRINGS = [
    ("scripts.txt", 0x14054E8D8),
    ("triggers.txt", 0x1405529B8),
    ("simple_triggers.txt", 0x1405529D0),
    ("module.ini", 0x14054C2C8),
    ("module.ini", 0x14055DA09),
    ("Unrecognized opcode %d", 0x14055B890),
    ("opcode %d out of range", 0x14056D2A0),
    ("Script [%d] %s", 0x14055C680),
    ("Unable to map script %s", 0x14055C690),
]

TABLES = [
    {
        "name": "mapped_script_names",
        "va": "0x140615ad0",
        "entries": 65,
        "element": "const char*",
        "first_entries": [
            "game_start",
            "game_quick_start",
            "game_set_multiplayer_mission_end",
            "game_get_console_command",
            "game_enable_cheat_menu",
            "game_event_party_encounter",
            "game_event_simulate_battle",
            "game_get_item_buy_price_factor",
        ],
        "last_entries": [
            "wse_initial_window_start",
            "wse_camp_button_clicked",
        ],
    },
]

TARGET = {
    "binary": "mb_warband_wse2_x64.exe",
    "release": "v1.1.5.5",
    "file_version": "1.1.5.5",
    "sha256": "d4e97d56981a6dab3de0348e3a0513ae200e7b5701b86732a4654b0aa4e29709",
    "format": "PE32+",
    "architecture": "x86_64",
    "image_base": "0x140000000",
    "pdb_guid": "fccf5e8d-4952-4a53-aa13-ac8b7ad38cfc",
    "pdb_age": 449,
}


def class_members(layout):
    members, methods = [], []
    for field in layout["fields"]:
        if field["kind"] in ("member", "static"):
            members.append({
                "name": field["name"],
                "offset": "0x%x" % field.get("offset", 0),
                "type": field["type"],
                "type_index": "0x%x" % field["type_index"],
                "storage": "static" if field["kind"] == "static" else "instance",
            })
        elif field["kind"] == "base":
            members.append({
                "name": field["name"],
                "offset": "0x%x" % field.get("offset", 0),
                "type": field["name"],
                "relation": "base",
            })
        elif field["kind"] == "method":
            methods.append(field["name"])
    return members, methods


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layouts", required=True)
    ap.add_argument("--types", required=True)
    ap.add_argument("--globals", required=True)
    ap.add_argument("--vtables", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--generated", default="2026-11-12")
    args = ap.parse_args()

    type_index = {}
    for row in json.load(open(args.types, encoding="utf-8"))["classes"]:
        type_index.setdefault(row["name"], row["type_index"])

    classes = {}
    for name in CLASSES:
        filename = LAYOUT_FILES.get(name, name + ".json")
        path = os.path.join(args.layouts, filename)
        if not os.path.exists(path):
            continue
        layout = json.load(open(path, encoding="utf-8"))["layout"]
        members, methods = class_members(layout)
        classes[name] = {
            "size": layout["size"],
            "type_index": "0x%x" % type_index.get(name, layout.get("field", 0) + 1),
            "kind": layout["kind"],
            "source": "pdb-tpi",
            "members": members,
            "methods": methods,
        }

    plain_classes = [cls for cls in CLASSES if cls not in LAYOUT_FILES]
    sym_filters = plain_classes + SYMBOL_TOKENS
    globals_rows = json.load(open(args.globals, encoding="utf-8"))["globals"]
    symbols = []
    for row in globals_rows:
        if row.get("kind") not in ("public", "global"):
            continue
        name = row["name"]
        if not any(tok in name for tok in sym_filters) and name not in GLOBAL_NAMES:
            continue
        entry = {"name": name, "kind": row["kind"], "source": "pdb-" + row["kind"]}
        if row.get("va"):
            entry["va"] = "0x%x" % row["va"]
        if row.get("type_index"):
            entry["type_index"] = "0x%x" % row["type_index"]
        symbols.append(entry)

    vtables = []
    for row in json.load(open(args.vtables, encoding="utf-8"))["vtables"]:
        cls = row.get("class_guess")
        if not cls or not re.match(r"^(rgl|mb|WSE|Parser@mu|\?\$WSEDynMultiArray)", cls):
            continue
        vtables.append({
            "va": "0x%x" % row["va"],
            "entries": row["entries"],
            "class": cls,
            "first_symbol": row.get("first", ""),
            "source": "pe-pointer-run+pdb-destructor" if row.get("first") else "pe-pointer-run",
        })

    payload = {
        "schema": "wse2-symbol-map/1",
        "generated": args.generated,
        "target": TARGET,
        "inputs": {
            "layouts": len(classes),
            "globals_scanned": len(globals_rows),
            "symbols_curated": len(symbols),
            "game_vtables": len(vtables),
        },
        "strings": [{"text": text, "va": "0x%x" % va} for text, va in STRINGS],
        "tables": TABLES,
        "classes": classes,
        "symbols": symbols,
        "vtables": vtables,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(payload, f, indent=1)
        f.write("\n")
    print("classes=%d symbols=%d vtables=%d -> %s" % (
        len(classes), len(symbols), len(vtables), args.out))


if __name__ == "__main__":
    main()
