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
import bisect
import json
import os
import re
import struct

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
    "mbStringManager",
    "mbLanguageManager",
    "rglWindowManager",
    "mbParticleSystemManager",
    "rglResource",
    "rglResourceManager",
    "rglTexture",
    "rglShader",
    "rglMaterial",
    "rglPhysicsMaterial",
    "rglBody",
    "rglBodyPart",
    "rglSphere",
    "rglCapsule",
    "rglQuad",
    "rglManifoldBody",
    "rglManifoldBodyFace",
    "rglCompositeBody",
    "rglSkeletonModel",
    "rglSkeletonModelBone",
    "rglSkeletonModelBody",
    "rglSkeleton",
    "rglBone",
    "rglQuaternion",
    "rglManifold",
    "rglManifoldFace",
    "rglAiMesh",
    "mbSkeletonManager",
    "mbSkeleton",
    "mbSkeletonBone",
    "mbGameScreen",
    "mbNotes",
    "mbEvent",
    "mbFadingOutEntity",
    "rglSkeletonAnimation",
    "rglSkeletonAnimationBone",
    "rglMeshVertex",
    "rglMeshVertexGroup",
    "rglMeshVertexGroupPair",
    "rglMeshVertexKey",
    "rglMeshFaceCorner",
    "rglMeshFace",
    "rglMetaMeshLod",
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
    "rglHashVector@VmbParty@@",
    "mbCoreGame",
    "mbAction",
    "mbActionSet",
    "mbConversationManager",
    # mbConversationWindow::updatePresentations (0x1400888c0) keeps prsntf_manual_end_only
    # presentations when closing the others; its methods own the mission-side presentations.
    "@mbConversationWindow@@",
    # Overlay widget behaviour behind overlay_set_size / create_*_button_overlay
    # (rglWidget::setScale 0x14031e7e0, rglImageWidget / rglGameButtonWidget::setScale).
    "@rglWidget@@",
    "@rglImageWidget@@",
    "@rglGameButtonWidget@@",
    "@rglImageButtonWidget@@",
    # Widget state-trigger / combo / slider / scroll behaviour and the window manager frame loop.
    "@rglContainerWidget@@",
    "@rglSimpleButtonWidget@@",
    "@rglSliderWidget@@",
    "@rglNumberBoxWidget@@",
    "@rglComboLabelWidget@@",
    "@rglComboButtonWidget@@",
    "@rglScrollableWidget@@",
    "@rglWindowManager@@",
    "@rglTextBoxWidget@@",
    "??0rglTextBoxWidget@@",
    "mbSentence",
    "mbFloraKind",
    "mbGroundSpec",
    "mbMapIcon",
    "mbMenu",
    "mbMesh",
    "mbMusicManager",
    "mbMusicTrack",
    "mbParticleSystem",
    "mbPostEffect",
    "mbPresentation",
    "mbSkin",
    "mbSkybox",
    "mbSound",
    "rglIni",
    "rglResource",
    "rglResourceManager",
    "rglTexture",
    "rglShader",
    "rglMaterial",
    "rglPhysicsMaterial",
    "rglBody",
    "rglSphere",
    "rglCapsule",
    "rglQuad",
    "rglManifoldBody",
    "rglCompositeBody",
    "rglSkeletonModel",
    "rglSkeletonModelBody",
    "rglSkeleton",
    "rglBone",
    "rglQuaternion",
    "rglIpo@VrglQuaternion",
    "rglManifold",
    "rglAiMesh",
    "mbSkeletonManager",
    "mbSkeleton",
    "mbSkeletonBone",
    "mbGameScreen",
    "mbMenuWindow",
    "mbNotes",
    "mbEvent",
    "mbFadingOutEntity",
    "rglStableVector@VmbEvent",
    "rglSkeletonAnimation",
    "rglMesh",
    "rglMeshFaceCorner",
    "rglMeshVertexKey",
    "rglMetaMesh",
    "mbCoreGame",
    "mbSavegame",
    "mbLoadSaveWindow",
    "rglOutputBinaryFileStreamWithChecksum",
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
    "g_basicGame",
    "g_skeletonManager",
    "g_timersTotal",
    "g_stringManager",
    "g_particleSystemManager",
    "g_windowManager",
    "g_languageManager",
    "g_skills",
    "g_levelTable",
    "s_randSeed",
    "g_resourceManager",
    "g_moduleVersion",
    "g_compatibleSavegameModuleVersion",
    "rglConfig::Campaign::fMapCameraMinZoom",
    "rglConfig::Campaign::fMapCameraMaxZoom",
    "?fMapCameraMinZoom@Campaign@rglConfig@@2MA",
    "?fMapCameraMaxZoom@Campaign@rglConfig@@2MA",
}

HELPER_SYMBOLS = {
    "?bShowCasualties@Messages@rglConfig@@2_NA",
    "?bShowCrosshair@Battle@rglConfig@@2_NA",
    "?bHorseFriendlyFire@Battle@rglConfig@@2_NA",
    "?bRagdollHeadshots@Physics@rglConfig@@2_NA",
    "?bCheatMode@General@rglConfig@@2_NA",
    "?bEditMode@General@rglConfig@@2_NA",
    "?doStep@mbLoadingWindow@@QEAA_NXZ",
    "??0mbMapWindow@@QEAA@XZ",
    "?frameMove@mbMapWindow@@UEAAXM@Z",
    "?readMap@mbGame@@QEAAXXZ",
    "?readSkeletons@mbGame@@QEAAXXZ",
    "?removeParty@mbGame@@QEAAXH@Z",
    "?addPartyToParty@mbGame@@QEAAXHH_N@Z",
    "?spawnProp@mbMission@@QEAAHHH@Z",
    "?clearScene@mbMission@@QEAAXXZ",
    "?setPruneTime@mbMissionObject@@QEAAXM@Z",
    "?initialize@mbMissionObject@@QEAAXXZ",
    "?setAlpha@rglEntity@@QEAAXI@Z",
    "?readSceneObjFile@mbSite@@QEAAXXZ",
    "?disconnectPlayer@mbnetData@@QEAAXH@Z",
    "?disconnectPlayer@mbnetNetworkServer@@QEAAXH@Z",
    "?startMission@mbnetData@@QEAAXXZ",
    "?mbCheckRegister@@YA_NH@Z",
    "?mbCheckParty@@YA_NH@Z",
    "?mbCheckString@@YA_NHH@Z",
    "?mbCheckPlayer@@YA_NH@Z",
    "?writePosToBuff@@YAXAEAVWSESimpleCharBuff@@PEBX@Z",
    "?DXUtil_Timer@@YANW4TIMER_COMMAND@@@Z",
    "?fmod@@YAMMM@Z",
    "?mupFmod@@YAMMM@Z",
    "??$rglClampExclusive@H@@YAHHHH@Z",
    "?pow@@YAMMM@Z",
    "?rglPow@@YAMMM@Z",
    "?rglAbs@@YAMM@Z",
    "?rglRound@@YAHM@Z",
    "?rglRound64@@YA_JM@Z",
    "?rglError@@YAXPEBDZZ",
    "?length@rglVector2@@QEBAMXZ",
    "?getAngle@rglVector2@@QEBAMXZ",
    "?rglRand@@YAHH@Z",
    "?mupDegToRad@@YAMM@Z",
    "?mupRadToDeg@@YAMM@Z",
    "?MD5@@YA_NPEBE_KQEAD@Z",
    "?Compare@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEBAHPEB_W@Z",
    "?CompareNoCase@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEBAHPEB_W@Z",
    "?Replace@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEAAHPEB_W0@Z",
    "?Find@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEBAHPEB_WH@Z",
    "?MakeUpper@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEAAAEAV12@XZ",
    "?MakeLower@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEAAAEAV12@XZ",
    "?TrimRight@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEAAAEAV12@XZ",
    "?TrimLeft@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEAAAEAV12@XZ",
    "?Trim@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEAAAEAV12@XZ",
    "?Right@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEBA?AV12@H@Z",
    "?Left@?$CStringT@_WV?$StrTraitATL@_WV?$ChTraitsCRT@_W@ATL@@@ATL@@@ATL@@QEBA?AV12@H@Z",
    "??0?$basic_regex@DV?$regex_traits@D@std@@@std@@QEAA@PEBDW4syntax_option_type@regex_constants@1@@Z",
    "??0?$regex_iterator@V?$_String_const_iterator@V?$_String_val@U?$_Simple_types@D@std@@@std@@@std@@DV?$regex_traits@D@2@@std@@QEAA@V?$_String_const_iterator@V?$_String_val@U?$_Simple_types@D@std@@@std@@@1@0AEBV?$basic_regex@DV?$regex_traits@D@std@@@1@W4match_flag_type@regex_constants@1@@Z",
    "??$sprintf_s@$0CAA@@@YAHAEAY0CAA@DPEBDZZ",
    "??$regex_match@DV?$regex_traits@D@std@@@std@@YA_NPEBDAEBV?$basic_regex@DV?$regex_traits@D@std@@@0@W4match_flag_type@regex_constants@0@@Z",
    "??$regex_search@DV?$regex_traits@D@std@@@std@@YA_NPEBDAEBV?$basic_regex@DV?$regex_traits@D@std@@@0@W4match_flag_type@regex_constants@0@@Z",
    "??$regex_replace@V?$regex_traits@D@std@@D@std@@YA?AV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@PEBDAEBV?$basic_regex@DV?$regex_traits@D@std@@@0@0W4match_flag_type@regex_constants@0@@Z",
    "iswalpha",
    "iswdigit",
    "iswspace",
    "_wtof",
    "atof",
    "_wcsicmp",
    "_wcsrev",
    "__imp_UrlUnescapeA",
    "__real@2edbe6ff",
    "__real@3c8efa35",
    "__real@447a0000",
    "?_Get_bits@?$_Rng_from_urng@IV?$mersenne_twister_engine@I$0CA@$0CHA@$0BIN@$0BP@$0JJAILANP@$0L@$0PPPPPPPP@$06$0JNCMFGIA@$0P@$0OPMGAAAA@$0BC@$0GMAHIJGF@@std@@@std@@AEAAIXZ",
    "?_Refill_lower@?$mersenne_twister@I$0CA@$0CHA@$0BIN@$0BP@$0JJAILANP@$0L@$06$0JNCMFGIA@$0P@$0OPMGAAAA@$0BC@@std@@IEAAXXZ",
    "?getProjectionMatrix@rglCamera@@QEBA?AU_D3DXMATRIXA16@@XZ",
    "?initDeviceObjects@rglEffectManager@@QEAAXXZ",
    "?reapplyScriptParams@rglEffectManager@@QEAAXPEAUID3DXEffect@@PEBD@Z",
    "?checkLoadingPostEffects@rglGraphicsContext@@QEAAXXZ",
    "?initEffectHandles@rglGraphicsContext@@QEAAXXZ",
    "?createBlendStates@rglGraphicsContext@@QEAAXXZ",
    "?createBlendState@rglGraphicsContext@@QEBAPEAVrglBlendState@@_NW4_D3DBLEND@@1W4_D3DBLENDOP@@@Z",
    "?setFogEnabled@rglGraphicsContext@@QEAAX_N@Z",
    "?setAlphaTestMode@rglGraphicsContext@@QEAAXH_N@Z",
    "?initDeviceObjects@rglTechnique@@QEAAXXZ",
    "?initEarlyZHandle@rglTechnique@@QEAAXXZ",
    "?setSourceContext@rglRenderContext@@QEAAXPEAV1@@Z",
    "__imp_D3DXMatrixPerspectiveOffCenterRH",
    "__imp_D3DXCreateEffectFromFileA",
    "__imp_D3DXMatrixOrthoOffCenterRH",
}

ARRAY_OBJECT_SIZES = {
    "g_timersTotal": 40,
    "g_skills": 9408,
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
    ("<>:\"/\\|?*", 0x14055AC20),
    (r'u"_"', 0x14055B3DC),
    (r'u" "', 0x14055B3E0),
    ("%.*f", 0x14055B3E4),
    (r'u"{"', 0x14055B3EC),
    (r'u">"', 0x14055B3F8),
    (r'u""', 0x14055BA68),
    ("%02x", 0x14056BCA8),
    ("load_resource", 0x14054C3B0),
    ("load_resource_nofast", 0x14054C3C0),
    ("load_mod_resource", 0x14054C3F0),
    ("load_module_resource", 0x14054C408),
    ("body", 0x14055C984),
    ("sphere", 0x14055E028),
    ("capsule", 0x14055E030),
    ("face", 0x14055E038),
    ("composite", 0x14055E040),
    ("manifold", 0x14055E050),
    ("core_shaders,core_textures,core_materials,core_ui_meshes,core_pictures,core_physics_materials", 0x14055EB10),
    ("Invalid vertex index %d in mesh %s", 0x1405657E8),
    ("Too many bones in mesh %s", 0x140565810),
    ("wse2_shaders", 0x1405681D8),
    ("rfver ", 0x14056839C),
    ("end", 0x1405683A4),
    ("mesh", 0x1405683A8),
    ("texture", 0x1405683B0),
    ("shader", 0x1405683B8),
    ("material", 0x1405683C0),
    ("skeleton", 0x1405683D0),
    ("skeleton_anim", 0x1405683E0),
    ("physics_material", 0x1405683F0),
    ("Unrecognized resource type %s", 0x140568408),
    ("Number of alternatives exceeds %d for shader %s", 0x1405693A8),
    (r"Textures\%s_%d.dds", 0x14056B380),
    (r"%sTextures\%s_%d.dds", 0x14056B398),
    (r"Textures\languages\%s\%s_%d.dds", 0x14056B3B0),
    (r"%sTextures\languages\%s\%s_%d.dds", 0x14056B3D0),
    (r"Textures\%s", 0x14056B3F8),
    (r"TexturesHD\%s", 0x14056B408),
    (r"%sTextures\%s", 0x14056B418),
    (r"Textures\languages\%s\%s", 0x14056B428),
    (r"%sTextures\languages\%s\%s.dds", 0x14056B448),
    # Slice P5 shader / effect loader strings (restored after 9cc4431 regenerated the map
    # from a list that lacked them).
    ("mb_2b.fxo", 0x140562058),
    ("mb_2a.fxo", 0x140562068),
    ("mb_wse2_3_0.fxo", 0x140562078),
    ("mb_wse2_2b.fxo", 0x140562088),
    ("mb_wse2_2a.fxo", 0x140562098),
    ("mb.fx", 0x1405620A8),
    ("EFFECT file=%s", 0x1405620B0),
    ("PostFx.fx", 0x140562C48),
    ("#if WSE2", 0x140562C58),
    ('#include "postFX_WSE2.fx"', 0x140562C70),
    ("postFX_WSE2.fx", 0x140562C90),
    ("earlyZ.fxo", 0x140566610),
    ("earlyz_wse2.fxo", 0x140566620),
    ("EARLYZ file=%s", 0x140566630),
    ("wse2_shaders.ini", 0x140567D70),
    (r"Resource\wse2_shaders.brf", 0x140568168),
    (r"CommonRes\wse2_shaders.brf", 0x140568188),
    ("Loading optional shader resource wse2_shaders", 0x1405681A8),
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


def extract_function_ranges(exe_path, curated_symbols, globals_rows):
    import pefile

    pe = pefile.PE(exe_path, fast_load=True)
    pe.parse_data_directories(
        directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
    )
    base = pe.OPTIONAL_HEADER.ImageBase
    secs = {s.Name.rstrip(b"\0").decode("latin1"): s for s in pe.sections}
    text = secs[".text"]
    text_lo = base + text.VirtualAddress
    text_hi = text_lo + text.Misc_VirtualSize

    raw_pdata = []
    parents = {}
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        b, end, ui = e.struct.BeginAddress, e.struct.EndAddress, e.struct.UnwindData
        raw_pdata.append((b, end))
        off = pe.get_offset_from_rva(ui)
        ver_flags, _prolog, codes, _frame = struct.unpack_from("<BBBB", pe.__data__, off)
        if (ver_flags >> 3) & 0x4:
            cnt = codes + (codes & 1)
            chain_off = off + 4 + cnt * 2
            pb, pend, _ = struct.unpack_from("<III", pe.__data__, chain_off)
            parents[(b, end)] = (pb, pend)

    def root_of(r):
        seen = set()
        while r in parents and r not in seen:
            seen.add(r)
            r = parents[r]
        return r

    pdata_by_root = {}
    pdata_begins = sorted(base + b for b, _ in raw_pdata)
    for b, end in raw_pdata:
        rb, _ = root_of((b, end))
        pdata_by_root.setdefault(base + rb, []).append((base + b, base + end))

    all_text_vas = sorted({
        r["va"] for r in globals_rows
        if r.get("va") and text_lo <= r["va"] < text_hi
    })

    curated_by_va = {}
    for entry in curated_symbols:
        if "va" in entry:
            va = int(entry["va"], 16)
            if text_lo <= va < text_hi:
                curated_by_va.setdefault(va, entry["name"])

    ranges = []
    seen_ranges = set()
    for va, sym_name in sorted(curated_by_va.items()):
        chunks = pdata_by_root.get(va)
        if chunks:
            for s, e in chunks:
                key = (s, e, va)
                if key not in seen_ranges:
                    seen_ranges.add(key)
                    ranges.append({
                        "start": "0x%x" % s,
                        "end": "0x%x" % e,
                        "va": "0x%x" % va,
                        "symbol": sym_name,
                    })
        else:
            idx = bisect.bisect_right(all_text_vas, va)
            next_sym = all_text_vas[idx] if idx < len(all_text_vas) else text_hi
            pidx = bisect.bisect_right(pdata_begins, va)
            next_pdata = pdata_begins[pidx] if pidx < len(pdata_begins) else text_hi
            end = min(next_sym, next_pdata, va + 0x1000)
            if end > va:
                key = (va, end, va)
                if key not in seen_ranges:
                    seen_ranges.add(key)
                    ranges.append({
                        "start": "0x%x" % va,
                        "end": "0x%x" % end,
                        "va": "0x%x" % va,
                        "symbol": sym_name,
                    })

    ranges.sort(key=lambda r: (int(r["start"], 16), int(r["end"], 16)))
    return ranges


def extract_global_objects(curated_symbols, types_path, pdb_path):
    types_by_idx = {}
    for row in json.load(open(types_path, encoding="utf-8"))["classes"]:
        types_by_idx[row["type_index"]] = row["size"]

    db = None
    if pdb_path:
        import pdb_symbols

        streams = pdb_symbols.msf_streams(pdb_path)
        db = pdb_symbols.TypeDb(pdb_symbols.parse_tpi(streams[2]))

    objects = []
    seen = set()
    for entry in curated_symbols:
        name = entry["name"]
        if entry.get("kind") not in ("global", "local") or "va" not in entry:
            continue
        if name not in GLOBAL_NAMES:
            continue
        va = int(entry["va"], 16)
        size = ARRAY_OBJECT_SIZES.get(name)
        ti = int(entry["type_index"], 16) if "type_index" in entry else None
        if size is None and db is not None and ti is not None:
            size = db.size_of(ti)
        if size is None and ti is not None:
            size = types_by_idx.get(ti)
        if size and size > 0 and (va, name) not in seen:
            seen.add((va, name))
            objects.append({
                "name": name,
                "va": "0x%x" % va,
                "size": size,
                "end": "0x%x" % (va + size),
            })

    objects.sort(key=lambda o: int(o["va"], 16))
    return objects


def extract_superseded_vas(old_globals_path, curated_exact_vas, globals_rows, sym_filters):
    raw = json.load(open(old_globals_path, encoding="utf-8"))
    old_rows = raw["globals"] if isinstance(raw, dict) else raw
    new_names_by_va = {}
    for r in globals_rows:
        if r.get("va"):
            new_names_by_va.setdefault(r["va"], set()).add(r["name"])

    superseded = set()
    for r in old_rows:
        va = r.get("va")
        if not va or va in curated_exact_vas:
            continue
        name = r["name"]
        if any(tok in name for tok in sym_filters) or name in GLOBAL_NAMES or name in HELPER_SYMBOLS:
            if name not in new_names_by_va.get(va, set()):
                superseded.add(va)

    return ["0x%x" % va for va in sorted(superseded)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layouts", required=True)
    ap.add_argument("--types", required=True)
    ap.add_argument("--globals", required=True)
    ap.add_argument("--vtables", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--generated", default="2026-11-12")
    ap.add_argument("--exe", help="Optional PE32+ executable for function_ranges extraction")
    ap.add_argument("--pdb", help="Optional PDB for exact array/pointer global_objects sizing")
    ap.add_argument("--old-globals", help="Optional superseded globals JSON for superseded_vas")
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
        kind = row.get("kind")
        name = row["name"]
        is_exact_curated = name in GLOBAL_NAMES or name in HELPER_SYMBOLS
        if kind not in ("public", "global") and not (kind == "local" and is_exact_curated):
            continue
        if not is_exact_curated and not any(tok in name for tok in sym_filters):
            continue
        entry = {"name": name, "kind": kind, "source": "pdb-" + kind}
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

    function_ranges = (
        extract_function_ranges(args.exe, symbols, globals_rows) if args.exe else []
    )
    global_objects = extract_global_objects(symbols, args.types, args.pdb)
    curated_exact_vas = (
        {int(s["va"], 16) for s in symbols if "va" in s}
        | {va for _text, va in STRINGS}
        | {int(t["va"], 16) for t in TABLES}
        | {int(v["va"], 16) for v in vtables}
    )
    superseded_vas = (
        extract_superseded_vas(args.old_globals, curated_exact_vas, globals_rows, sym_filters)
        if args.old_globals
        else []
    )

    payload = {
        "schema": "wse2-symbol-map/1",
        "generated": args.generated,
        "target": TARGET,
        "inputs": {
            "layouts": len(classes),
            "globals_scanned": len(globals_rows),
            "symbols_curated": len(symbols),
            "game_vtables": len(vtables),
            "function_ranges": len(function_ranges),
            "global_objects": len(global_objects),
            "superseded_vas": len(superseded_vas),
        },
        "strings": [{"text": text, "va": "0x%x" % va} for text, va in STRINGS],
        "tables": TABLES,
        "classes": classes,
        "symbols": symbols,
        "vtables": vtables,
        "function_ranges": function_ranges,
        "global_objects": global_objects,
        "superseded_vas": superseded_vas,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(payload, f, indent=1)
        f.write("\n")
    print(
        "classes=%d symbols=%d vtables=%d ranges=%d globals=%d superseded=%d -> %s"
        % (
            len(classes),
            len(symbols),
            len(vtables),
            len(function_ranges),
            len(global_objects),
            len(superseded_vas),
            args.out,
        )
    )


if __name__ == "__main__":
    main()
