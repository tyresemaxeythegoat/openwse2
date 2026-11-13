# STATUS

Last updated: 2026-11-13.
Phase: **2 — Core Data Structures & Module System VM** (Phase 1 triage is complete; see `ROADMAP.md`).

This file is the **only** place that holds live numbers (gate results, coverage counts). Other
documents link here instead of repeating them — see the ownership table in `AGENTS.md` §4.

## 1. At a glance

| Area | State |
| --- | --- |
| Evidence baseline | WSE2 **v1.1.5.5** x64 (PDB + PE + Ghidra 12.1.4). The 2026-10-14 build is superseded. |
| Host gate | **PASS** on Windows (Zig clang++) and Linux CI (`clang++ 18.1.3` CMake/CTest) — 6 suites, 63 test functions |
| Android arm64 gate | **PASS (CI)** — `.github/workflows/ci.yml` (`android-arm64`, NDK `30.0.16248370` Clang 21, `aarch64-linux-android29`) compiles all 11 headers standalone + CMake build; on-device runtime tests remain unrun |
| WSE extended opcodes | 440/440 mapped, **222/440 implemented** |
| Native (non-WSE) opcodes | **1,053 live handlers mapped** (`16` control-flow `0–29` + `1,037` switch `30–2767`, plus `6` `noop_true` and `1` SDK-named fallback `1004`; `3` unnamed live handlers `53, 2180, 2749`); **49/1,053 implemented** (`16` control-flow incl. 4 zero-iteration stubs + `33` switch) |
| Module loaders | `scripts.txt`, `triggers.txt`, `simple_triggers.txt` only |
| World state | In-memory model (`WseWorldState`) exists; nothing loads it from module files yet |
| Lua 5.1 layer | Not started (hook call sites documented only) |
| Renderer / audio / input / Android app | Not started in the clean-room engine (legacy PanVK loader work lives in `runtime/android/`) |

## 2. Gates

| Gate | Result | Detail |
| --- | --- | --- |
| Host C++20, Windows | PASS | `engine/build_host.ps1`, Zig `clang++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror`: `operand`, `trigger`, `savegame`, `script`, `wse`, `block_vm` |
| Host C++20, Linux | PASS (CI) | `.github/workflows/ci.yml` (`ubuntu-24.04`, Clang `18.1.3`, CMake + CTest `-DWSE_BUILD_HEADER_CHECKS=ON`): all 11 standalone header checks + 6/6 suites passing |
| Android NDK `aarch64-linux-android29` | PASS (CI) | `.github/workflows/ci.yml` (`ubuntu-24.04`, NDK `30.0.16248370` / Clang `21.0.0`, `-std=c++20 -O2 -Wall -Wextra -Werror`): all 11 headers pass standalone compile + CMake cross-compile build; on-device runtime tests remain unrun |
| Host PanVK loader/surface test (legacy) | PASS (recorded) | `host loader tests: PASS`; independent of the reconstructed engine |
| CI | PASS | `.github/workflows/ci.yml` (`Engine CI`): runs `host-linux` and `android-arm64` on push and PR |

## 3. Coverage

### WSE extended opcodes (222/440)

Counted from the distinct `case WseOpcode::…` labels in `execute_wse()` against `docs/wse_opcodes.json`.
The per-opcode handler notes live in `engine/README.md`; this table is the only tally.

| Block | Family | Done | Remaining examples |
| ---: | --- | ---: | --- |
| 2800 | shifts / xor / not | 10/10 | — |
| 2900 | player | 0/10 | `player_set_skin`, `player_set_banner_id` |
| 3000 | registers, script context, slots, timers, misc | 36/46 | `set_forced_lod`, `send_message_to_url_advanced`, `order_flag_is_active`, `play_bink_file` |
| 3100 | input keys | 0/3 | `game_key_get_key`, `key_released` |
| 3200 | dictionaries | 23/23 | — |
| 3300 | agent | 0/61 | `agent_get_item_modifier`, `agent_get_animation_progress` |
| 3400 | multiplayer messaging | 0/17 | `multiplayer_send_chat_message_to_player` |
| 3500 | server admin | 0/21 | `server_set_password_admin`, `server_map_rotation_get_count` |
| 3600 | mission / props / particles | 6/26 | `particle_system_remove`, `prop_instance_set_forced_lod` |
| 3700 | troop | 10/10 | — |
| 3800 | item / cur_item | 14/14 | — |
| 3900 | party | 0/12 | `party_stack_get_experience`, `party_has_flag` |
| 4100 | position vectors | 4/5 | `position_align_to_ground` |
| 4200 | strings | 41/47 | `str_store_multiplayer_profile_name`, `str_store_overlay_text` |
| 4300 | options | 6/6 | — |
| 4400 | floating point | 39/39 | — |
| 4500 | scene | 0/23 | `scene_set_flags`, `scene_set_water_level` |
| 4600 | edit mode | 10/10 | — |
| 4700 | reload / material | 0/4 | `update_material`, `reload_item_kinds` |
| 4800 | menus | 0/4 | `menu_create_new`, `menu_add_item` |
| 4900 | overlays / presentations | 0/8 | `overlay_get_val`, `presentation_activate` |
| 5000 | dynamic arrays | 23/23 | — |
| 5100 | Lua bridge | 0/15 | `lua_get_top`, `lua_set_top` |
| 5200 | skins / skeletons | 0/3 | `skin_set_blood_color` |

The pure-computation families are finished. Every remaining family needs engine state that does
not exist yet (agents, parties, mission, network, UI, Lua), so the count will not move much until
those containers are reconstructed.

### Native opcodes (`block_vm.h`, `docs/native_opcodes.json`)

Generated from `mbStatement::execute` jump table `0x1401e22f0` (`2,738` slots), `mbStatementBlock` control-flow dispatch (`0–29`), public `header_operations.py` (`1,057` unique IDs in `0–2767`), and live `block_vm.h` scan via `tools/extract_native_opcodes.py` (`docs/NATIVE_OPCODES.md`):

- Control flow `0–29` (**16/16 live** implemented): `call_script` 1, `try_end`/`try_begin`/`else_try` 3–5, `try_for_range` 6, `try_for_range_backwards` 7, `break_loop` 8, `continue_loop` 9, `try_for_parties` `0xB` (11), `try_for_agents` `0xC` (12), `try_for_prop_instances` `0x10` (16), `try_for_players` `0x11` (17), `try_for_dict_keys` `0x12` (18), `store_script_param*` 21–23. (`0xB` / `0xC` swapped naming in `block_vm.h` was fixed to match `startLoop` `0x1401f7516`/`0x1401f732e` and `header_operations.py`.) The four entity iterators (`11, 12, 16, 17`) run **zero iterations** until world entity containers are loaded.
- Switch table `30–2767` (`2,738` slots):
  - **1,037** live unique `switch_handler` `.text` targets (incl. **3** unnamed live handlers: `53` reading `rglConfig::General::bCheatMode`, `2180` counting main-party prisoner stacks, `2749` agent helper), of which **33** are implemented in `block_vm.h`: comparisons `30–33`, shifts/arithmetic/math/assign `2100–2101`, `2105–2117`, `2119–2131`, `2133`.
  - **6** shared `noop_true` stubs jumping directly to `0x1401e226b` (`473`, `477`, `1070`, `1104`, `1302`, `1491`).
  - **1,695** `execute_wse_fallback` slots jumping to `0x1401e223e`: **1** SDK-named (`1004` `race_completed_by_player` / `is_vanilla_warband`, returning `false` under WSE2) and **1,694** unassigned holes.
- Every other native opcode currently returns `true` and increments `VmContext::unimplementedCount`. A condition built on an unimplemented opcode therefore **passes silently**; results from running real module scripts are not trustworthy until `unimplementedCount` reaches zero (`ROADMAP.md` B6).

### Demand ranking (`tools/demand_rank.py`, Backlog B3)

Measured on the `3,467,512`-byte retail `Native` `scripts.txt` (**1,755** scripts, **131,080** statements, **653** distinct opcodes; counts only — no module content; `triggers.txt` and `simple_triggers.txt` were absent from that local build output):

- **Covered today:** `86,807` statements (`86,760` implemented native calls, including `221` zero-iteration entity-loop calls; `24` `noop_true`/fallback calls; `23` implemented WSE calls). `212 / 1,755` scripts use zero unimplemented opcodes.
- **Unimplemented demand:** `44,268` native calls across **596** distinct native opcodes, plus `5` WSE calls across `3` unimplemented opcodes (`3340` `agent_set_action_speed_modifier` ×2, `3341` `agent_get_action_speed_modifier` ×2, `3904` `party_switch_stacks` ×1).
- **Entity-iterator zero-iteration stubs used:** `try_for_agents` (`12`) ×190 calls in 110 scripts; `try_for_parties` (`11`) ×31 calls in 30 scripts.

| Top K unimplemented native | Unimplemented calls covered | Share of missing native calls | Fully covered scripts |
| ---: | ---: | ---: | ---: |
| 10 | 27,747 | 62.68% | 403 |
| 25 | 32,732 | 73.94% | 537 |
| 50 | 36,502 | 82.46% | 725 |
| 100 | 39,954 | 90.25% | 948 |

Top categories by unimplemented native calls (`scripts.txt`): `entity_slots` (**27,324** across 45 opcodes — **61.7%** of all missing native calls), `strings` (3,144 / 21), `troops` (2,210 / 52), `parties` (2,060 / 86), `agents` (1,915 / 83), `ui_presentations_and_menus` (1,751 / 40), `positions_and_math` (1,580 / 32), `scenes_props_and_mission` (914 / 33), `game_state_and_stores` (859 / 26), `arithmetic_registers_and_vars` (803 / 5).

<details>
<summary>Top 25 unimplemented native opcodes in Native scripts.txt (counts only)</summary>

| Rank | Opcode | Name | Category | Calls | Scripts |
| ---: | ---: | --- | --- | ---: | ---: |
| 1 | 500 | `troop_set_slot` | `entity_slots` | 10,624 | 296 |
| 2 | 507 | `item_set_slot` | `entity_slots` | 7,815 | 63 |
| 3 | 501 | `party_set_slot` | `entity_slots` | 2,347 | 103 |
| 4 | 2320 | `str_store_string` | `strings` | 1,975 | 227 |
| 5 | 520 | `troop_get_slot` | `entity_slots` | 1,248 | 397 |
| 6 | 502 | `faction_set_slot` | `entity_slots` | 829 | 41 |
| 7 | 540 | `troop_slot_eq` | `entity_slots` | 811 | 263 |
| 8 | 2136 | `store_random_in_range` | `arithmetic_registers_and_vars` | 771 | 229 |
| 9 | 1106 | `display_message` | `ui_presentations_and_menus` | 724 | 254 |
| 10 | 1560 | `troop_set_health` | `troops` | 603 | 18 |
| 11 | 521 | `party_get_slot` | `entity_slots` | 463 | 170 |
| 12 | 541 | `party_slot_eq` | `entity_slots` | 459 | 141 |
| 13 | 2319 | `str_clear` | `strings` | 422 | 90 |
| 14 | 505 | `agent_set_slot` | `entity_slots` | 395 | 90 |
| 15 | 729 | `position_set_x` | `positions_and_math` | 392 | 78 |
| 16 | 730 | `position_set_y` | `positions_and_math` | 362 | 76 |
| 17 | 1263 | `set_visitor` | `scenes_props_and_mission` | 357 | 16 |
| 18 | 1704 | `agent_is_human` | `agents` | 345 | 141 |
| 19 | 2322 | `str_store_troop_name` | `strings` | 321 | 149 |
| 20 | 525 | `agent_get_slot` | `entity_slots` | 284 | 80 |
| 21 | 1507 | `troop_is_hero` | `troops` | 274 | 176 |
| 22 | 1718 | `agent_get_troop_id` | `agents` | 246 | 137 |
| 23 | 2204 | `store_faction_of_party` | `parties` | 226 | 126 |
| 24 | 522 | `faction_get_slot` | `entity_slots` | 222 | 96 |
| 25 | 2173 | `store_troop_faction` | `troops` | 217 | 116 |

</details>

### Tests

| Suite | Test functions |
| --- | ---: |
| `operand_test.cpp` | 6 |
| `trigger_test.cpp` | 10 |
| `savegame_test.cpp` | 8 |
| `script_test.cpp` | 5 |
| `wse_test.cpp` | 25 |
| `block_vm_test.cpp` | 9 |

## 4. Blocked

| Blocker | Impact | Way out |
| --- | --- | --- |
| No local Android NDK/SDK, `adb`, or device | CI runs the NDK arm64 compile gate on Linux; on-device Android runtime tests remain unrun | Keep `.github/workflows/ci.yml` enforcing the arm64 build; add `adb`/device validation later in Phase 3/4 |
| REA not registered with OpenCode | REA/Ghidra tools are not exposed through MCP; the CLI works from `%TEMP%\opencode\tools\rea\node_modules\rea-agents` | Keep using the CLI |
| `tools/vtables.py` is not in the repository | `tools/build_symbol_map.py` names it as the source of `vtables_x64.json`, so `docs/SYMBOL_MAP.json` cannot be regenerated from a clean checkout | Commit the script or fold it into `tools/pe_evidence.py` (backlog B7) |

## 5. Next concrete tasks (ordered)

1. **Generate WSE implementation status (`ROADMAP.md` backlog B4):** have `tools/extract_wse_opcodes.py`
   scan `execute_wse.h` so `docs/wse_opcodes.json` and `STATUS.md` §3 tallies stay machine-generated.
2. **Split `execute_wse.h` & `wse_test.cpp` (backlog B5)** and **surface per-opcode unimplemented native
   counters (backlog B6)** so Phase 2H fails loudly on any missing native opcode.
3. **Demand-driven native opcodes & world loaders:** reconstruct the `entity_slots` (`500–599`) and
   top `strings` (`2319–2322`) / `store_random_in_range` (`2136`) / `position_set_{x,y}` (`729–730`)
   handlers along with `troops.txt`/`item_kinds1.txt`/`factions.txt`/`parties.txt`/`strings.txt`
   loaders feeding `WseWorldState` (unlocking >62% of unimplemented calls in `scripts.txt`).
4. Continue remaining `executeWSE` handlers and entity containers (`agent`, `party`, `mission`) in
   demand order rather than numeric order.

## 6. Open documentation questions

These were found during the 2026-11-13 review and need the original evidence files to settle:

- `docs/ARCHITECTURE.md` §1 calls the derived artifacts `types_new.json` / `globals_new.json` /
  `vtables_new.json`; §9 and `tools/build_symbol_map.py` call them `*_x64.json`. It is unclear
  whether the §9 hashes are for the v1.1.5.5 or the superseded build.
- Vtable counts disagree: 73 game-class vtables (ARCHITECTURE §1), 75 (§3), 76 in
  `docs/SYMBOL_MAP.json`.
- `docs/ARCHITECTURE.md` §2 (type inventory) still holds the superseded 2026-10-14 prefix counts.
- 274 distinct image addresses are cited in `engine/include`; 98 of them appear in
  `docs/SYMBOL_MAP.json` and 85 appear in neither the symbol map nor `docs/ARCHITECTURE.md`
  (mostly mid-function handler sites in `execute_wse.h`). Nothing checks citations automatically.

## 7. Evidence & analysis ledger

| Check | Result | Evidence |
| --- | --- | --- |
| Target extraction | PASS | v1.1.5.5 `mb_warband_wse2_x64.exe` 7,060,992 B, SHA-256 `d4e97d56…4e29709`; official `WSE2.zip` `c45197e8…276e28b1` |
| REA/Ghidra analysis | PASS | v1.1.5.5 analysis (`ev_e323351d…` / `ev_fa7fc7d6…`): 9 segments, 20,276 procedures, 7,448 strings; image base `0x140000000`; PDB `fccf5e8d…8cfc` age 449 |
| PE import inventory | PASS (v1.1.5.5) | `ev_a4af0bca5d107b626454d91f4c69c2aef5d4f21b466cbb174333d7c8cd687fc6`: 23 DLLs (21 static + 2 delay), 482 + 64 = 546 functions |
| PDB parse (MSF 7.0/TPI/DBI) | PASS | 156,846 TPI records, 15,363 sized layouts |
| PDB symbol map | PASS | 48,658 symbols with VAs (`globals_new.json`) |
| Vtable scan | PASS | 279 pointer runs; 207 class-identified; 73 `rgl*`/`mb*`/`WSE*` |
| Tracking documents | PASS | `AGENTS.md`, `ROADMAP.md`, `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (58 classes, 2,804 symbols, 76 vtables; adds `WSEObjectContainer`, `WSEObjectType`, `WSEArraySortOptions`, `WSEBasicDynMultiArray`, `WSEDynMultiArray<int/rglString/rglTransform>`, `mu::Parser`, `mu::ParserBase`, plus `mbTroop`, `mbItemKind`, `mbItemKindVariation`, `mbSkill`, `mbPartyTemplate`, `mbFaction`, `mbQuest`, `mbSite`, `mbParty`, `mbnetPlayer`, `mbAgent`, `mbMissionObject`, `mbSceneProp`, `rglMesh`, `rglMetaMesh`), v1.1.5.5 target |
| v1.1.5.5 REA VM/stream batches | PASS | 75 procedures across 9 `decompile_all` exports (`stmt_core` `ev_3ebcea85…` 4/4, `stmt_block` `ev_ffc38aa5…` 10/10, `script` `ev_8d37a0c8…` 4/4, `simple_trigger` `ev_3157906d…` 12/12, `trigger` `ev_f62843ac…` 12/12, `game_scripts` `ev_fc9bd1a4…` 4/4, `game_triggers` `ev_052f2d86…` 2/2, `streams_bin` `ev_8cc48986…` 9/9, `streams_txt` `ev_1e3c2566…` 18/18), plus `read_bytes`/`0x140615ad0` (`ev_f09662c6…`/`ev_2ddf7a62…`) and 6 single-procedure exports |
| Superseded 2026-10-14 REA runs | PASS (superseded) | Historical 2026-10-14 entries kept for provenance only: `ev_7cd99ccc…157d7a` (`mbScript` 5), `ev_8d91b175…99b11` (`mbStatement` 14), `ev_25469ff4…84af8` (3), `ev_e3530f99…e8d05` (`Trigger` 52), `ev_cebc7ae0…e8c8d3` (`rglTextStream` 18), `ev_24eecfa4…f18c0b6` (`rglBinaryStream` 9), `ev_130d8023…2c62e6` (2) |
| Binary savegame format | PASS | `rglBinaryStream` raw little-endian 8/4/1-byte values; strings are `int32` length + bytes; no header or checksum |
| Module stream identification | PASS | `0x1405651e0` = `rglInputTextFileStream` (readers at `+0x58/+0x70/+0x78/+0x80/+0x90`); `0x1405653e0` = `rglOutputTextFileStream` writers; module files are decimal token text |
| Host PanVK loader/surface test | PASS | `host loader tests: PASS` (legacy renderer path, unaffected) |
| Portable engine host tests | PASS | `operand tests: PASS`, `trigger tests: PASS`, `savegame tests: PASS`, `script tests: PASS`, `wse tests: PASS`, `block_vm tests: PASS`, `engine host tests: PASS` (Zig clang++ `-std=c++20 -O2 -Wall -Wextra -Werror`) |
| Statement-block VM & trigger/script bridge | PASS | `engine/include/wse/vm/block_vm.h` reconstructed and host-tested (`analyze`, `execute`, `startLoop`/`endLoop`/`canLoop`, `try_for_dict_keys` `0x12`, core native ops `30–33`/`2100–2133`, `call_script` up to depth 255, `store_script_param*`, `this_or_next`/`neg`, script & trigger execution) |
| Game-world runtime state | PASS | `engine/include/wse/game/world_state.h` models PDB-verified entity/slot, troop, skill, item/mesh, mission, config, and edit-mode containers (`WseWorldState` wired into `VmContext::world` and `WseVmState::world`) |
| WSE extended opcode map | PASS | v1.1.5.5 `executeWSE` (`0x1401e4dc0`) pseudocode `ev_2aaa96a8…5cf0f9`: 440/440 in-range operations 2800–5202 (431 switch + 9 equality), no unknown or unhandled opcode; 8,844-line dump |
| Retail `scripts.txt` parse | PASS | Full 3,467,512-byte sample: 1,755 scripts, every token consumed, 52 of the 64 then-known names (the rest are WSE-only scripts absent from that module); v1.1.5.5 adds a 65th name |
| WSE2 v1.1.5.5 release delta | PASS (inventory) | Official `WSE2.zip` SHA-256 `c45197e8…276e28b1`; x64 exe 7,060,992 B with file version 1.1.5.5; SDK adds 7 operations (3911, 4517–4522), no renumbering; 440 in-range ops; mapped script table grows to 65 with `wse_camp_button_clicked` |
| Ghidra toolchain health | PASS | `rea doctor`: ghidra 12.1.4, JDK 25.0.5, native decompiler and native authority all healthy |

## 8. Environment notes

- `%TEMP%` extraction directories are wiped on server restarts. Node 26, JDK 25, and Ghidra 12.1.4
  are now extracted to the persistent `C:\rea-tools` tree; the zips remain in `%TEMP%\opencode\tools`.
- Required environment for CLI runs:
  `JAVA_HOME=C:\rea-tools\jdk-25.0.5+8`,
  `GHIDRA_INSTALL_DIR=C:\rea-tools\ghidra_12.1.4\ghidra_12.1.4_PUBLIC`.
- `msdia140.dll` is now COM-registered system-wide (`regsvr32`, elevated) from
  `C:\rea-tools\ghidra_12.1.4\ghidra_12.1.4_PUBLIC\Ghidra\Features\PDB\os\win_x86_64\`. Ghidra can
  load the retail PDB directly; the earlier hanging `RegSvr32` dialogs were orphaned manual
  registration attempts and are resolved.
- `decompile-all` refuses a pre-created output directory; let the tool create it. Use `--resume`
  only when a previous export manifest exists.
- Each export re-runs full Ghidra auto-analysis (~15–20 min). Do not run two exports concurrently
  on this 8 GB machine; a second OpenCode session using Ghidra drives free RAM below 1 GB.

## Appendix A — Facts confirmed by pseudocode / disassembly

Reference material, kept verbatim. Grouped facts for the same subsystem also appear in
`docs/ARCHITECTURE.md`; when the two disagree, re-check the evidence export before trusting either.

- `mbScriptManager::readStream` (`0x14020f7c0`): two `readString` calls (discarded), two 32-bit reads,
  the second into `m_numScripts`; frees the old array, allocates `8 + count*176`, constructs
  `mbScript[]`, calls `mbScript::readStream(index)` per entry, then `mapScripts()`.
- `mbScript::readStream` (`0x14020f6a0`) order: `m_id` (`readString`), `m_interval`
  (`readFloat` @ `+0x48`), `mbStatementBlock::readStream` (@ `+0x50`), then sets
  `m_statementBlock.m_id` to `rglFormat("Script [%d] %s", index, m_id)`.
- `mbScriptManager::mapScripts` (`0x14020f9a0`): 65-entry static name table at `0x140615ad0`
  (`game_start` … `wse_initial_window_start`, `wse_camp_button_clicked`), linear match on `m_id` with the first script in file
  order winning, writes the index or `-1` into `m_mappedScripts` (`0x14069facc`), and calls `rglInfo`
  with `"Unable to map script %s"` on failure. All 65 names are transcribed in `script.h` and were
  verified byte-for-byte against the table pointers in the v1.1.5.5 image.
- Module files are whitespace-delimited decimal text, not binary. `mbGame::readTriggers`
  (`0x1400ccf20`) and `readSimpleTriggers` (`0x1400cd2d0`) build `rglInputTextFileStream`
  (vftable `0x1405651e0`), open `triggers.txt`/`simple_triggers.txt`, discard three header tokens
  (`triggersfile version 1`), and pass the stream to the manager.
- `rglStream` (160 B) holds `m_path` at `+0x8`, `m_cursor` at `+0x54`, and the whole file as
  `m_contents` at `+0x58`. `rglTextStream` (160 B) supplies the token readers but emits no
  standalone vftable. `rglInputTextFileStream` (176 B, vftable `0x1405651e0`) adds `FILE* m_file` at
  `+0xA0` and `bool m_closed` at `+0xA8`; `rglOutputTextFileStream` (176 B, vftable `0x1405653e0`)
  adds the same plus `m_fileOpen` at `+0xA9`. Reader slots: `+0x50 readStream`, `+0x58 readFloat`,
  `+0x60 readBool`, `+0x68 readUInt64`, `+0x70 readInt64`, `+0x78/+0x80` the folded 32-bit parser,
  `+0x88 readByte`, `+0x90 readString`.
- `rglTextStream` reader behaviour (v1.1.5.5 `ev_1e3c2566…0fdb0b`, 18/18 functions): `readFloat` is `strtod(token)` cast to
  float; `readBool` is `_stricmp("true"/"false")` else `strtol(token) != 0`; the integer readers use
  manual decimal loops (15-character cap, leading `-`); `readString` copies the raw token, with no
  quote handling. Writers append a single `0x20` after each value and write booleans as `0`/`1`.
  The portable reader parses strictly and rejects malformed tokens as a safety deviation.
- `mbStatementBlock::readStream` (`0x1401f6600`): reads `int32` statement count, allocates
  `count * 0x110`, then per statement reads `uint32` opcode, `int32` operand count, and that many
  `int64` operand tokens; sets every `m_endStatement` (`+0x108`) to `-1`, clears `m_analyzed`, and
  seeds `m_numLocalVariablesUsed` to `0x400`. **No bounds check** on either count.
- `mbStatementBlock` layout: `m_statements` `+0x0`, `m_numStatements` `+0x8`, `m_analyzed` `+0xc`,
  `m_numLocalVariablesUsed` `+0x10`, `m_luaHookRef` `+0x14`, `m_id` `+0x18` (96 bytes total).
- `analyze` (`0x1401f6740`): matches opcodes `3/4/5` and loop starters `6,7,0xb,0xc,0x10,0x11,0x12`
  with two 128-entry stacks, writes `m_endStatement` jump targets, caps nesting at 128, and computes
  `m_numLocalVariablesUsed` from operand tag `0x11`, clamped to `[0x20, 0x400]`.
- Operand encoding: 7-bit tag in bits 56–62, 56-bit payload. Tags `0` immediate, `1` global variable
  array `g_globalVariables` (`0x140662c30`, unchecked), `2` global `mbSlots` vector
  (`0x140662c18` begin / `0x140662c20` end / `0x140662c28` capacity, read bounds-checked),
  `0x11` local variable. Negative 64-bit values are untagged immediates.
- `mbStatement::parseOperand` (`0x1401c6070`) implements the decoder: `<= 0` is an immediate,
  otherwise tag = `(value >> 56) & 0x7f` and payload = `value & 0x00FFFFFFFFFFFFFF`.
- `mbStatement::returnValue` (`0x1401c6100`) treats operand 0 as the destination lvalue and writes
  the result to the global array (tag 1), `mbSlots::set` (tag 2), or the local array (tag `0x11`);
  other tags raise `"Illegal lvalue in statement"`.
- `mbStatement` constructor (`0x1401c6060`) only initializes `m_endStatement` to `-1`; `readStream`
  fills the opcode and operands.
- `mbStatement::execute` (`0x1401c6150`): parses operands, masks opcode with `0x0FFFFFFF`, calls
  `WSELuaManager::OnOperationExecute` when `operationHookTable[opcode] != -2`, then dispatches
  opcodes `30..2767` through the 2,738-entry RVA jump table at `0x1401e22f0` and opcodes `>= 2768`
  to `executeWSE`. Bit 31 inverts the result; bit 30 arms the stop latch (`g_skipStatement`,
  `0x14061eec4`).
- `docs/wse_opcodes.json` / `docs/WSE_OPCODES.md` / generated `wse_opcodes.h` cover all 440 SDK
  operations in the extended range 2800–5202. 431 are switch cases in `executeWSE`; the other 9 are
  equality-tested handlers at 2800, 3000, 3300, 3600, 3900, 4200, 4400, 4700, and 5000. No in-range
  SDK operation lacks a handler. The map is generated from the v1.1.5.5 pseudocode
  (`ev_2aaa96a845fb5e0466129d8f6726e8172c13981585db951f98d8235a415cf0f9`).
- The 2800–2809 group is proven by the pseudocode switch at lines 638–665 (`0xaf1`–`0xaf9`) and the
  equality branch at line 859 (`0xaf0`): `val_shr`/`store_shr`
  cast to signed before `>>` (arithmetic); `val_lshr`/`store_lshr` shift the `ulonglong` operand
  directly (logical); `val_shl`/`store_shl` shift left; `val_xor`/`store_xor` combine operands 0/1 or
  1/2; `val_not`/`store_not` complement operand 0 or 1. Every shift masks the count with `& 0x3F`.
- The 3000–3002 group is proven by the equality branch at line 628 (`register_get`) and the switch at
  lines 870–877: `register_get` checks `param_4[1]` with `mbCheckRegister`, then loads
  `(&g_globalVariables)[index]`; `register_set` checks `param_4[0]` and stores `param_3[1]` into the
  same bank. The bank is `g_basicGame+0x270` (`0x140662c30`, confirmed by
  `mov qword ptr [r14 + rcx*8 + 0x270], rax` at `0x1401e552d`). `store_wse_version` ignores its value
  operand: selector 0 yields 5, selector 1 yields `0x483` (1155), and any other selector uses the
  zeroed `r15` default (`0x1401e553a`–`0x1401e555b`).
- The script-context query group is proven by five separate handler sites:
  `store_current_trigger` (`0x1401e5917`) returns `param_6` only when it is `<= 0` (`cmovle`),
  and `mbStatement::execute` passes its `r8d` context argument as `param_6` at `0x1401e223e`/
  `0x1401e2262`; WSE's `GetCurrentTrigger` uses the same negative-context rule.
  `store_num_return_values` (`0x1401e5979`) reads `0x140698ae8`. `store_return_value`
  (`0x1401e5985`) accepts a 1-based index 1–16, requires it to be `<= 0x140698ae8`, and reads
  `0x140698af0 + (index-1)*8`; out-of-range/unset indices log at `0x14055afe8`/`0x14055b008` and
  return true without storing. `store_application_time` (`0x1401e63a4`) calls
  `FUN_1400b9690(5)` (QPC seconds since the app-time baseline), converts to float, multiplies by
  `1000.0f` (`0x140576b90`), and rounds through `rglRound64`. `store_cur_mission_template_no`
  (`0x1401ea6b2`) reads `[0x14061f078]+0x9ae8` or yields -1 when the pointer is null.
- **Script state, timers, RNG, and profiler (`3015`, `3020–3022`, `3027–3028`, `3034`, `3040–3043`, v1.1.5.5 `executeWSE`):**
  `return_values` (`0xbc7`) populates up to 16 slots (`g_returnValues`) and `g_numReturnValues`;
  `mtsrand`/`mtrand` (`0xbcc`/`0xbcd`, helpers `0x1402a24f0`/`0x1402a25e0`) back `rglMersenneTwister`
  (`0x6556`, 5,000 B), while `set_random_seed` (`0xbda`) updates the C RNG seed; `get_time`
  (`0xbce`, `_localtime64`/`_gmtime64` at `0x1404981d0`/`0x1404987b4`) formats calendar fields and
  `timer_reset`/`timer_get_elapsed_time` (`0xbd3`/`0xbd4`) convert ticks using `100 kHz` resolution;
  `profiler_{start,stop,is_recording,mark}` (`0xbe0..0xbe3`, `WSEProfiling` `0xaf0a`, 66,072 B)
  control profiling state via `0x14034f7d0`, `0x14034ff40`, and `0x1403508a0`.
- **Dictionary operations (`3200–3222`) & `try_for_dict_keys` (`0x12`, v1.1.5.5):**
  `dict_*` (`0xc80..0xc96`) store key/value maps owned by `WSEObjectManager` (`0x1b4d3`, 64 B) with
  LIFO IDs and support `.wsedict` binary, `rglSectionlessIni` (`0x5264`, 16 B) INI, JSON
  (`0x1402a97f0`/`0x1402a9f70`), and URL-encoded JSON (`0x1402aaea0`/`0x1402ab370`) plus
  path validation (`0x140071d10`); `core_op::kTryForDictKeys` (`0x12`) iterates sorted dictionary keys
  across `mbStatementBlock::{startLoop,endLoop,canLoop}` (`0x1401f7290`, `0x1401f7910`, `0x1401f7f00`).
- **Position vector & FP load operations (`4100`, `4102–4104`, `4402–4404`, v1.1.5.5):**
  `position_get_vector_to_position` (`0x1004`), `position_get_length` (`0x1006`),
  `get_dot_product_of_positions` (`0x1007`), `get_cross_product_of_positions` (`0x1008`), and
  `fld_pos_{x,y,z}` (`0x1132..0x1134`) operate over `rglTransform` (`0x34ac`, 64 B) / `rglVector4`
  (`0x32d9`, 16 B) via helpers `0x1402b5be0`/`0x1402b5ea0` and `0x140318b40..0x140318ce0`.
- The string group covers `4200`–`4224`, `4233`–`4234`, `4236`, `4238`, and `4240`–`4244`, proven by
  the equality branch at `0x1068` (`0x1401ed1bd`) and the switch cases decoded from the jump
  table at `0x1401ed2ef` (`0x1069`–`0x1094`). String
  operands resolve through `0x140219c70`: tag 0 is the writable 128-entry string register table at
  `0x140665030` (stride 0x48, assigned by `0x14030fd60` and copied by `0x140310970`), tag 0x16 is
  the local register bank (`[0x140621778]`, count `[0x140621784]`, stride 0x90), and tag 3 or an
  unknown tag is the global bank (`[0x140621770]`, count `[0x140621780]`, stride 0x90).
  `mbCheckStringRegister` (`0x1401c51f0`) bounds tags 3/0x16 by their counts, tag 0 by `0x7f`, and
  accepts unknown tags; tag-0 destination registers use `mbCheckRegister` (`0x1401c4e60`).
  `str_equals`/`contains`/`starts_with`/`ends_with` use `wcscmp` (`0x1401f86b0`) or `_wcsicmp`
  (`0x1401f86f0`) when operand 2 is nonzero; `str_contains` lower-cases both strings
  (`0x1401f8b40`) and treats an empty needle as not contained. `str_is_alpha/digit/whitespace`
  call `iswalpha`/`iswdigit`/`iswspace` (`0x140498854`/`0x140498860`/`0x14049886c`), scan the whole
  string when the index is absent or -1, and leave the initial true result on an out-of-bounds
  index. `str_index_of`/`str_last_index_of` take operand 3 as the start (default 0) and operand 4
  as the end (default length-1); `str_last_index_of` mirrors the retail loop, which searches again
  before testing `end <= start`. `str_get_char` returns a UTF-16 code unit or stores nothing when
  the index is out of bounds. `str_to_num` calls `_wtof` (`0x140498878`), scales by
  `DAT_140662bb8` (100) when operand 2 is nonzero, and truncates with `cvttsd2si`.
  `str_compare` (`0x1401ee042`) normalizes to `-1/0/1` via `0x1401f86b0` or tail-jumps to `_wcsicmp`
  (`0x1404989d4`) for the raw folded code-unit difference when operand 3 is nonzero. `str_split`
  (`0x1401ee178`) writes pieces into tag-0 registers (capped at `0x7f`), skips empty pieces on
  operand 4, honors `max` (operand 5) only when >5 operands are present, and avoids the retail
  infinite loop on an empty delimiter. `str_sort` (`0x1401ee3eb`) sorts via `0x140200c20`/
  `0x140200c00` and reverses copy-back when operand 3 is nonzero. `str_store_lower`/`upper`/`trim`/
  `replace`/`substring`/`reverse`/`join` and `4223`/`4224` call `0x1401f8b40`/`0x1401f8ac0`/
  `0x1401f8e20`/`0x1401f8d00`/`0x1401f8c40`/`0x1401f8710`/`0x1401f8e40`/`0x1404992b0`/`0x1401f90b0`
  and use `DAT_14055b3e0` (`u" "`) and `DAT_14055b3dc` (`u"_"`). `str_store_md5` (`0x1401ee956`)
  hashes UTF-8 (`0xfde9`) bytes via `CryptCreateHash(0x8003)` (`0x140330510`) and formats 16 digest
  bytes with `"%02x"` (`0x14056bca8`). `str_store_float` (`0x1401ef3a8`) formats `(double)fp[op1]`
  with `"%.*f"` (`0x14055b3e4`) via `0x140200c40`, defaulting negative or missing precision to `5`.
  `str_sanitize` (`0x1401ef436`) converts tag-0 operand 0 via `MultiByteToWideChar(0xfde9)`
  (`0x1403100d0`) and strips `u"{"`, `u"}"`, `u"<"`, `u">"` (`0x14055b3ec`..`0x14055b3f8`).
  `str_is_integer` (`0x1401ef55f`) trims via `0x1401f8e20`, rejects empty or bare `'-'`, and checks
  digits with `iswdigit` (`0x140498860`). `str_store_server_password_rcon` (`4238`, `0x108e`) maps in
  the jump table directly to the shared no-op exit `0x1401f49c1`. `str_regex_*` (`4240`–`4243`,
  `0x1401ef741`–`0x1401efbc0`) use ECMAScript `std::regex` over UTF-8 (`0x1400163b0`, `0x140200c70`,
  `0x140200db0`, `0x1401f92f0`, `0x140200df0`), taking `<start_string_register>` as operand 0 in
  `str_regex_get_matches` (`4242`); exceptions evaluate false/no-store. `str_decode_url` (`4244`,
  `0x1401efd4d`) calls `SHLWAPI.dll!UrlUnescapeA` (`0x140501818`, `dwFlags=0`) in `0x1403108b0`.
- Options flags `4300`–`4305` (`0x10cc`–`0x10d1`, `0x1401efff9`–`0x1401f003b`) get/set the booleans
  `verbose_casualties` (`0x1406173b7`), `cheat_mode` (`0x14061ef50`), and `realistic_headshots`
  (`0x14061ef39`) via `WseVmState::options`.
- The floating-point cases at lines 5192 and 7881–8380 index the 128-float array `DAT_140698718`
  (`g_basicGame+0x35d58`), adding `4401` `fld_str` (`0x1401f00c7`), which parses operand 1 with
  `atof` (`0x1404989cc`).
  `mbCheckRegister` (`0x1401c4e60`) is `cmp ecx,0x7f; ja` (unsigned 0–127); a failed check returns
  true without doing anything. The fixed-point scale (`g_basicGame+0x1f8`, `0x140662bb8`) is set to
  `0x64` (100) at `0x140070ffa` inside `mbBasicGame::initialize` (`0x140070f90`); `fld` divides by
  it, `fst` multiplies by it.
- `rglRound64` (`0x1402a1910`) calls `floorf` (`0x1404d7f78`), subtracts to get the fraction, and adds
  one only when the fraction is greater than `0.5f` (constant at `0x14057647c`). `rglAbs`
  (`0x1402a1850`) clears the sign bit; `feq`/`fge`/`fle` compare through it with `FLT_EPSILON`.
- `fclamp` orders its two bounds first (`min`/`max` of operands 2 and 3), then returns the lower bound,
  the value, or the upper bound. `fdiv`/`fmod` report `"Division by zero"`/`"Modulo by zero"` and
  leave the destination unchanged.
- The math call sites prove the argument order: `fpow` loads operand 2 into `xmm1` and operand 1 into
  `xmm0` at `0x1401f08db`/`0x1401f08e5` (`pow(base, exponent)`); `fatan2` does the same at
  `0x1401f0ca8`/`0x1401f0cb2` (`atan2(y, x)`); `fmod` loads operand 2 as the divisor and operand 1 as
  the dividend at `0x1401f09f5`/`0x1401f0a1e`. The one-operand helpers read `fp[operand 1]` and the
  shared tail at `0x1401f062e` stores `xmm0` through operand 0.
- `flog` (line 8201, `0x1401f0968`–`0x1401f099f`): fewer than three operands selects base 10
  (`mov ebx,0xa`); otherwise the base is the int32 literal in operand 2. A base below 2 jumps to the
  logger with `"invalid logarithm base %d"` and returns true without storing. The result is
  `log10(fp[operand 1]) / log10((float)base)`.
- `feval` (`4437`, `0x1155`, line 8353): resolves its string operand, runs `mbBasicGame::parseString`
  (`0x1400720b0`, unconditionally mapping literal `'^'` at `0x140072338` to `'\n'` while leaving
  expanded `{sN}` contents un-reparsed), and evaluates with `mu::Parser` (`0x1a146`, 376 B; `SetExpr`
  `0x1400a9980`, `Eval` `0x1400ad5a0`, unary `'-'` `prINFIX=6` at `0x1400a8310` below `'^'`, `log`
  and `ln` both bound to `logf` `0x1400a6c20`, `rint` bound to `floorf(x + 0.5)` at `0x1400a6c50`).
  The portable implementation compiles the full AST before evaluation so syntax/arity/lvalue errors
  leave `fp[0..127]` and `positions[0..127]` unchanged.
- Dynamic arrays (`5000` `array_create` equality branch line 6427; `5001`–`5022` `0x1389`–`0x139e`
  switch lines 6802–7510): share `WSEObjectManager` (`0x1b4d3`, 64 B) / `WSEObjectContainer` (`0x18774`,
  16 B) / `WSEObjectType` (`0x18026`, 32 B) with dictionaries via unified `WseObjectManager` (type-checked
  object lookups), backing `WSEBasicDynMultiArray` (`0x163ff`, 48 B) and `WSEDynMultiArray<int/rglString/rglTransform>`
  (`0x66db`/`0x677c`/`0x681f`, 48 B) across 1–14 dimensions. `.wsearray` save/load (`0x1401c5910`/`0x1401c59f0`)
  stores `rglTransform` as 9 floats `[o.xyz, f.xyz, u.xyz]` and calls `orthonormalize()` on load;
  `array_sort`/`array_sort_custom` (`WSEArraySortOptions` `0x18fb1`, 8 B) use bottom-up natural merge
  sort (`0x1401fdd80`, aborting without mutation after `n` double-passes); `array_eq..array_le`
  (`0x1401f58a0`, `0x1401f5d30`–`0x1401f606b`) normalize position copies before comparing all 12 floats;
  and `block_vm.h` tracks `currentDepth` so `array_sort_custom` invokes comparator scripts via
  `wse_invoke_script_callback`.
- WSE2 v1.1.5.5 keeps every existing opcode number and adds seven SDK operations, all present in the
  recovered dispatch: `party_get_current_terrain_variant` 3911 (line 5020),
  `scene_set_region_variant` 4517, `scene_set_height_fog` 4518, `scene_set_water_waves` 4519,
  `scene_set_flora_wind` 4520, `scene_rebake_sky_cube` 4521, and `scene_set_weather_direction` 4522
  (lines 8658–8725).
- `mbStatementBlock::analyze` (`0x1401f6740`): maintains two 128-entry stacks (`tryStack`, `startStack`) so `else_try` sets `endStatement = elseIdx` on preceding unresolved inner statements while keeping `startStack[sp - 1]` anchored at the opener until `try_end` resolves the opener, `else_try`s, and final branch statements; computes `m_numLocalVariablesUsed` clamped to `[32, 1024]`.
- `mbStatementBlock::execute` (`0x1401f69d0`): resets `g_skipStatement` once at block entry (`0x1401f6cab`), interpreter loop with `0x1C08`-byte frame, 128 × `0x38` loop frames (`mbLoop`), locals zeroing, block-level Lua hook, `call_script` recursion into `m_scripts[index] + 0x50` (stride `0xB0`, max depth 255), and special handling for opcodes `3/4/5/8/9`, `0x15/0x16/0x17`. In `startLoop`, failing initial `canLoop` jumps directly to `stmt.endStatement` (`0x1401f78ac`) so `endLoop` pops the loop frame cleanly.
- Native core ops & `mbBasicGame`: `mbBasicGame` (`0xadd3`, 221,616 B) stores `m_globalVariables` (`mbSlots`) at `+0x258` and `m_registers` (`__int64[128]`) at `+0x270` (`0x140662c30`), confirming operand tag `1` and WSE `register_{get,set}` share one register bank. Disassembly of core ops confirms `val_div` (`2108`) / `store_div` (`2123`) write sentinel `40000000` (`0x2625a00`) on division by zero and return `true`, `val_mod` (`2109`) / `store_mod` (`2119`) leave operand 0 unchanged on modulo by zero, and `val_clamp` (`2112`) clamps to half-open `[lower, upper)` (`upper - 1`).
- `WSELuaManager::OnOperationMgrExecute` (`0x14033d090`): fetches the hook from the Lua registry,
  pushes operands as integers, `lua_pcall(L, nargs, LUA_MULTRET, 0)`, and interprets the boolean
  result as an override or handled flag (`WSELuaManager::OnStatementExecute` at `0x14033cdd0`).
- `mbTriggerManager::readStream` (`0x140244810`): reads `m_numTriggers`, allocates
  `count * 0x100`, seeds all three `rglAppTimer`s per trigger with `m_defaultTimerNo` and
  `g_timersTotal[m_defaultTimerNo]` (`0x14061f030`), then calls `mbTrigger::readStream`.
- `mbTrigger::readStream` (`0x140244540`): three `readFloat` intervals (`+0x0/+0x4/+0x8`), the
  conditions block (`+0x10`), the consequences block (`+0x70`), then ids
  `"%s Trigger [%d] Conditions"` / `"%s Trigger [%d] Consequences"`.
- `mbSimpleTriggerManager::readStream` (`0x14020fe60`): the same shape with `count * 0x78` records
  and one timer each; `mbSimpleTrigger::readStream` (`0x14020fab0`) reads one float, the statement
  block, and formats `"%s Trigger [%d]"`.
- `mbGame::readTriggers` passes the prefix `"Game"` (`0x1405529c8`); `readSimpleTriggers` passes
  `"Simple"` (`0x1405529e4`).
- `mbTriggerManager::execute` (`0x140244cd0`) **inlines** the `mbTrigger` state machine; the PDB
  declares `mbTrigger::execute`, but no separate public symbol is emitted. The integer argument is a
  trigger event key: non-negative keys run interval triggers; negative keys match
  `interval_key(checkInterval)`. Confirmed call sites pass `-112` (exit status), `-105`/`-106`
  (ranged attack), `-26`/`-56` (death), `-27` (knockdown), `-28` (blow), `-110`/`-109` (movement and
  collision capsules), `-102` (turn), and `-113` (action sound); frame code passes `1` and `0x10`.
- Trigger statuses are `0` idle, `1` waiting for delay, `2` waiting for rearm. Timers advance in
  `g_timersTotal` units of 100000/second and elapsed is `(float)(now - value) / 100000.0`. Rearm
  expiry resets the check timer; idle conditions success starts the delay; delay expiry runs the
  consequences; a matching negative event key runs the consequences immediately.
- Savegames use `rglBinaryStream` (vtable `0x14055c780`, 30 slots). Writers store raw
  little-endian values (8 bytes for `writeInt64`, 4 for `writeInt32`/`writeFloat`, 1 for
  `writeBool`/`writeByte`); strings are an `int32` byte count followed by the bytes. There is no
  header, version, or checksum. `mbTriggerManager::writeSavegameStream` (`0x140244b60`) writes an
  `int32` count, then `int32` status and the `int64` check/delay/rearm timer values per trigger.
  `readSavegameStream` (`0x140244970`) reads the same order; when the file count is larger it calls
  `rglError` (`0x14032f450`) with `"Load error: Number of triggers in file: %d, exceeds %d"` (a
  message box that returns) and still reads and discards the surplus entries.
  `mbSimpleTriggerManager::readSavegameStream` (`0x14020ffa0`) reads one `int64` timer value per
  record with the analogous `"Load error: Number of simple triggers in file: %d, exceeds %d"`.
  The retail readers do not bounds-check; the portable reader rejects truncation.
