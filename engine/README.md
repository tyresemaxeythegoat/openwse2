# Portable engine sources

Clean-room C++20 reconstruction of the WSE2 engine. Code here is written from the evidence in
`docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json`; it contains no lifted machine code.

This file describes **what each module implements and how it deviates from retail**. Coverage
counts and gate results are kept only in [`../STATUS.md`](../STATUS.md); the plan is in
[`../ROADMAP.md`](../ROADMAP.md).

## Layout

```
engine/
  include/wse/core/   math.h, encoding.h, json.h              pure STL math, UTF/MD5/URL/LE, JSON helpers
  include/wse/io/     text_stream.h, binary_stream.h          module text + savegame streams
  include/wse/vm/     operand.h, slots.h, statement.h         statement encoding
                      block_vm.h, native_*.h                  block interpreter + native opcodes
                      wse_state.h, execute_wse.h              WSE VM state & top-level dispatcher
                      lua_runtime.h, lua_manager.h            abstract Lua runtime & factory interface
                      ops/{bitwise,system,world,string,fp_expr,fp,dict,array,position,lua_bridge}.h
                      wse_opcodes.h (generated)               WSE extended opcode enum
  include/wse/game/   script.h, trigger.h, world_state.h,    managers, module loaders, campaign helpers, headless runner, audio bridge, and state model
                      world_subsystem_types.h, world_campaign_helpers.h,
                      module_{loader_common,world_loaders,asset_loaders,mission_loaders,bundle}.h,
                      headless_runner.h, audio_runtime.h, world_audio.h, world_input.h
  include/wse/platform/ vfs.h, saf_vfs.h, clock.h,         platform seam (port infrastructure, SAF Vfs, minimal RHI, stereo audio mixer, input & touch overlay, sensors/thermal/FPS performance)
                      sleep.h, log.h, rhi.h, driver_package.h,
                      rhi_brf.h, audio.h, input.h, performance.h
  include/wse/asset/  brf.h, dds.h, shader.h,               .brf / .dds / WAV+OGG+MP3 asset decoders, module resource catalog, shader/material/SPIR-V pipeline
                      audio.h, audio_codecs.h
  src/audio/          wse_audio_internal.hpp,               `stb_vorbis` + `dr_mp3` codec backend (`wse_audio`, isolated from public headers)
                      wse_audio_{vendor,codecs}.cpp
  src/lua/            lua_bitop.*, lua_globals_script.hpp,    stock MIT Lua 5.1.5 WSE runtime (`wse_lua`, `<lua.h>` isolated here)
                      wse_lua_{internal.hpp,ops,callbacks,manager}.cpp
  tools/              wse_headless.cpp, wse_brf.cpp           headless module CLI validator (`Modules/Native`) + `.brf`/`.dds` inspector
  tests/              one *_test.cpp per suite (+ wse_test_common.h),
                      plus wse_selftest.{h,cpp} and selftest_host_test.cpp, no framework
  fixtures/           hand-written module snippets (never retail files)
  build_host.ps1      Windows host gate
```

The core engine headers under `include/wse/` remain header-only; the Lua runtime (`src/lua/`) compiles into static library `wse_lua` + stock MIT `lua51`, and the compressed audio codecs (`src/audio/`) compile into `wse_audio` + `wse_audio_vendor` (`stb_vorbis` + `dr_mp3`) via CMake (`../CMakeLists.txt`), CI (`.github/workflows/ci.yml`), or `engine/build_host.ps1`.

## Current modules

| File | Reconstructs | Evidence |
| --- | --- | --- |
| `include/wse/io/text_stream.h` | whitespace token reader and typed decimal parsers | `rglInputTextFileStream` vftable `0x1405651e0`, `rglTextStream` readers `0x1402afa80`–`0x1402afe60`, `mbGame::readTriggers` `0x1400ccf20` |
| `include/wse/io/binary_stream.h` | little-endian savegame reader/writer | `rglBinaryStream` vftable `0x14055c780`; writers `0x1402ae980/0x1402ae9d0/0x1402aea30`; readers `0x1402aebe0/0x1402aec00/0x1402aec20` |
| `include/wse/vm/operand.h` | tagged operand decode and lvalue write-back | `mbStatement::parseOperand` `0x1401c6070`, `mbStatement::returnValue` `0x1401c6100`, `mbStatementBlock::execute` `0x1401f69d0`, `analyze` `0x1401f6740` |
| `include/wse/vm/slots.h` | `mbSlots` (`std::vector<int64_t>`) get/set semantics | PDB type `0x1b0f8`; `parseOperand` bounds check; `returnValue` write path |
| `include/wse/vm/statement.h` | statement record and text reader | `mbStatementBlock::readStream` `0x1401f6600`, PDB type `0xb91e` |
| `include/wse/game/world_state.h` | PDB-backed `WseWorldState` entity/slot, skill, troop, item/meta-mesh, faction, quest/note, site/visitor, party/stack, player, agent, prop, team, presentation/overlay, battle event, mission timer, config/var, and edit-mode containers & validators | `mbTroop`, `mbItemKind`, `mbItemKindVariation`, `mbSkill`, `mbPartyTemplate`, `mbFaction`, `mbQuest`, `mbSite`, `mbParty`, `mbnetPlayer`, `mbAgent`, `mbMissionObject`, `mbSceneProp`, `mbTeam`, `rglMesh`, `rglMetaMesh`, `mbGame`, `mbMission` |
| `include/wse/game/world_campaign_helpers.h` | clean-room troop XP/inventory/merchandise/equipment and party member/prisoner/template/attachment helpers for `WseWorldState` | see `world_state.h` layouts |
| `include/wse/vm/block_vm.h` | statement-block VM, loop frames (`try_for_parties` `0xB`, `try_for_agents` `0xC`, `try_for_prop_instances` `0x10`, `try_for_players` `0x11`, `try_for_dict_keys` `0x12`), `NativeDispatch` + per-opcode `unimplementedOpcodeCounts` + `failOnUnimplemented`, `WseVmState`/`WseWorldState` wiring, recursive `currentDepth` / preserved `currentCaller` / `wse_invoke_script_callback` (`array_sort_custom`), script/trigger VM bridge | `0x1401c6150`, `0x1401f6740`–`0x1401f7f00`, `0x1401f7692`/`0x1401f7dde`, `0x1400dbde0`/`0x1400dbe70`, `0x140244cd0`, `0x14020fb80`/`0x1402101a0` |
| `include/wse/vm/native_ops.h` | core + Milestone 1 native dispatcher (`dispatch_native_statement` + `dispatch_native_statement_ext`, `145` switch opcodes): `33` core comparisons/math/assign (`30–33`, `2100–2133`), `44` `entity_slots`, `35` `positions_and_math`, `8` arithmetic/RNG/trig/gold, `19` `strings`, `display_message`, `troop_is_hero`, `troop_set_health`, `agent_is_human`, `store_troop_faction`, `store_faction_of_party`, entity validity filters | `0x14006f990`–`0x14032ffb0` |
| `include/wse/vm/native_entity_ops.h` | Milestone 2 entity/campaign native dispatcher (`dispatch_native_entity_statement`, `108` switch opcodes): player/MP (`47`, `394–408`, `430`), quests (`200–204`, `1113`, `1280–1286`), audio/ground/UI/overlays (`599–600`, `791`, `900–947`, `1107`), sites/factions (`1232`, `1261–1270`), troops (`1505–1550`, `2170–2176`), parties/campaign (`1601–1685`, `2156–2281`) | `0x1401c6548`–`0x1401de32d` |
| `include/wse/vm/native_mission_ops.h` | Milestone 2 mission/combat native dispatcher (`dispatch_native_mission_statement`, `66` switch opcodes): agent getters/setters/predicates/AI/combat (`1700–1789`), team & division control (`1790–1796`), scene/props/mission control (`1804–2075`), and mission timers (`2370–2372`) | `0x1401d36ee`–`0x1401df390` |
| `include/wse/vm/native_campaign_ops.h` | Milestone 3 campaign/world/troop/party native dispatcher (`dispatch_native_campaign_statement`, `156` switch opcodes): campaign state/achievements/multiplayer/positions (`36–793`), mission/spawning/troops/items (`1003–1570`), parties/stacks/morale (`1604–1694`) | `0x1401c6448`–`0x1401d5496` |
| `include/wse/game/world_subsystem_types.h` | PDB-backed subsystem structs (`WseFaceKeys`, `WseLightingState`, `WseSkyboxState`, `WsePostFxState`, `WseAudioRuntimeState`, `WseNetworkRuntimeState`, `WseDialogState`, `WseTableauRuntimeState`, `WseInputState`, `WseRayObstacle`, `WseMiscRuntimeState`) + `native_f2i_saturate` and `push_bounded` | `mbFaceKeys` `0x1400c5d10`–`0x1400c5dc0`, `mbGame`, `mbMission` |
| `include/wse/vm/native_campaign_ops_2.h` | Milestone 3 combat/item/mission/store/face-key native dispatcher (`dispatch_native_campaign_2_statement`, `128` switch opcodes): agent combat/items/teams/props (`1695–2099`), stores/relations/timers/strings (`2154–2379`), item getters/predicates & face keys (`2700–2751`) | `0x1401d3509`–`0x1401e1517` |
| `include/wse/vm/native_world_ops.h` | Milestone 4 world/face-key/input/troop/party native dispatcher (`dispatch_native_world_statement`, `69` switch opcodes incl. `53` `is_cheat_mode_enabled`, `2180` `store_main_party_wounded`, `2749` `str_store_agent_face_keys`) | `0x1401c659c`–`0x1401e2150` |
| `include/wse/vm/native_world_ops_2.h` | Milestone 4 mission-combat/audio-tableau/store native dispatcher (`dispatch_native_world_2_statement`, `69` switch opcodes across `mission_combat_and_teams`, `audio_particles_and_tableau`, and `game_state_and_stores`) | `0x1401c6626`–`0x1401df605` |
| `include/wse/vm/native_world_ops_3.h` | Milestone 4 `other_native` dispatcher (`dispatch_native_world_3_statement`, `80` switch opcodes covering input/options, spawn effectors/bans, MP/UI/difficulty, dialog/notes/quests/encounters, mission/tableau/talk, lighting/skybox/postfx) | `0x1401c69e2`–`0x1401dfbbb` |
| `include/wse/game/trigger.h` | trigger/simple-trigger records, loaders, savegames, and event-key runtime state machines | `0x140244810`, `0x140244540`, `0x14020fe60`, `0x14020fab0`, `0x1400ccf20`, `0x1400cd2d0`, `0x140244970`, `0x140244b60`, `0x14020ffa0`, `0x140244cd0`, `0x14020fb80`, `0x1402101a0` |
| `include/wse/game/script.h` | `mbScript`/`mbScriptManager` readers, 65-entry name table, and script mapping | `0x14020f6a0`, `0x14020f7c0`, `0x14020f9a0`; globals `0x14069fac0`/`0x14069fac8`/`0x14069facc`; table `0x140615ad0`; formats `0x14055c680`/`0x14055c690` |
| `include/wse/game/module_loader_common.h` | `ModuleLoadError`, token/version header helpers, BOM/comment-stripping INI parsers, and VFS text loader helpers (strictly via `wse::platform::Vfs` + `wse::io::TokenReader`); `[section]` headers are recognised after comment stripping, and `read_wrapping_u32_token` wraps negative literals in unsigned arithmetic | `rglIni` (`0x1402a6d80`), `rglSectionlessIni`, `mbCoreGame` |
| `include/wse/game/module_world_loaders.h` | world/campaign loaders (`skills`, `factions`, `troops` v1/v2, `item_kinds` v1/v3, `party_templates`, `parties`, `scenes`, `quests`, `info_pages`, `strings`, `quick_strings`, `game_variables`, `module.ini`, `module_config.ini`, `Data/item_modifiers.txt`) | `0x1402a6d80`, `0x140093e20`, v1.1.5.5 stream readers cited in header |
| `include/wse/game/module_asset_loaders.h` | asset/visual loaders (`meshes`, `actions`, `skins` (`0x140213690`), `sounds` v2/v3, `music`, `particle_systems`, `postfx`, `Data/{flora_kinds,ground_specs,skyboxes}.txt`) | v1.1.5.5 stream readers cited in header |
| `include/wse/game/module_mission_loaders.h` | interactive/mission loaders (`map_icons`, `scene_props`, `menus`, `presentations`, `mission_templates`, `tableau_materials`, `conversation` v1/v2) | v1.1.5.5 stream readers cited in header |
| `include/wse/game/module_bundle.h` | `WseOwnedStringBanks`, `ModuleBundle`, `ModuleLoadOptions`, and `load_module()` orchestrator populating `WseWorldState`, `ScriptManager`, triggers, and `VmContext` string banks; `load_module()` first resets module-derived `WseWorldState` (safety deviation: a reload never inherits the previous module's optional files), and the bundle is move-only because `VmContext` holds views into it | `mbGame::readModuleFiles` cited in header |
| `include/wse/game/headless_runner.h` | **port infrastructure:** `HeadlessRunOptions`, `HeadlessRunReport`, `HeadlessSession`, and `run_headless_module()` / `run_headless_session()` executing `load_module`, `game_start`, and simulated trigger frames via `Vfs` and `LogSink`; unimplemented opcodes stop and fail the run only when `fail_on_unimplemented` is set (otherwise they are counted and reported) | clean-room orchestration over `module_bundle.h`, `trigger.h`, `block_vm.h` |
| `include/wse/asset/{brf,dds,shader}.h` | `.brf` container decoder (all 8 resource types, `MetaMesh` LOD grouper, `module.ini` resource catalog), `.dds` parser/RGBA8 decoder (`BC1`–`BC5`, uncompressed/`L8`/`L16`/`A8L8`/`A8`, `DX10`) + Warband `Vfs` texture resolver, and P5 effect search / `wse2_shaders.{brf,ini}` / `rglShader` suitability / `rglMaterial` state / `GLShaders` + WSE2 SPIR-V 1.0 cache pipeline | `rglResourceManager`, `rglMesh`, `rglMetaMesh`, `rglTexture`, `rglShader`, `rglMaterial`, `rglPhysicsMaterial`, `rglBody`, `rglSkeletonModel`, `rglSkeletonAnimation`, `rglGraphicsContext::initEffect`, `rglPostProcess::initEffect` |
| `include/wse/asset/{audio,audio_codecs}.h`, `src/audio/` | RIFF/WAVE PCM (`8`/`16`/`24`/`32`-bit & IEEE float/`0xFFFE`) decoder, retail 4-step sound & 2-step music path resolvers, `mbSoundSample` / `mbMusicTrack` flag/priority/volume/culture helpers, and static `wse_audio` (`stb_vorbis` v1.24 + `dr_mp3` v0.7.5) memory + streaming decoder backend isolated from public headers | `mbSoundSample::readStream` `0x1403317c0`, `mbSoundSample::playSound` `0x140331cb0`, `mbSoundManager::readStream` `0x1403322e0`, `mbMusicTrack::readStream` `0x140290710`, `mbMusicManager::readStream` `0x140290ad0` |
| `include/wse/game/{audio_runtime,world_audio,world_input}.h` | non-owning `WseAudioRuntime` VM hook + `WorldAudioBridge` (`599..604`, `609`, `615..616`, `1750`, `1808`, `1881..1882`, `3618`), plus `WorldInputBridge` syncing `WseInputManager` into `WseWorldState` (`65`, `70..73`, `75`, `77..78`, `3100..3102`) | clean-room bridge over `0x140057010`, `0x1400e3900`, `0x140154340`, `0x140290c40`, `0x140332ac0`, `0x1401c6752`, `0x1401e6a42` |
| `tools/wse_headless.cpp` | portable Phase 2H CLI runner (`<warband_root> [module_rel_dir] [--frames N] [--step-ticks T] [--allow-unimplemented]`) using `HostVfs` + `StderrLog` | Phase 2H headless `Native` exit runner |
| `tools/wse_brf.cpp` | portable Slice P3/P5 / Phase 2G CLI inspector (`--brf <file>`, `--dds <file>`, `--dir <dir>`, `--root <warband_root> [--module <name>]`, `--json`) using `HostVfs` | Slice P3/P5 `.brf` / `.dds` / shader & material corpus validator |
| `include/wse/vm/wse_opcodes.h` | generated enum of the 440 WSE-extended operation names in the retail dispatch range | `mbStatement::executeWSE` `0x1401e4dc0`; names from `header_operations_addon.py` |
| `include/wse/core/{math,encoding,json}.h` | pure STL math vectors/transforms/MT19937, UTF-8/UTF-16/MD5/URL/path/LE codecs, minimal JSON parser/serializer | `rglTransform`, `rglMersenneTwister`, `UrlEscapeA`/`UrlUnescapeA`, `Json::Reader`/`Writer` |
| `include/wse/vm/wse_state.h` | opcode mask, operand/effect structs, `WseStringBanks`, `WseOptionsState`, `WseObjectManager`, `parseString`, shared file helpers | `executeWSE` `0x1401e4dc0`, `0x1400720b0`, `0x1401c6150` |
| `include/wse/vm/ops/{bitwise,system,world,string,fp_expr,fp,dict,array,position}.h` | per-family `executeWSE` dispatch handlers (2800–2809, 3000–3045/3619/3622/4300–4305/4606, 3340–3341, 3600–3911/4600–4609, 4200–4246, `feval` 4400–4438, dict 3200–3222, array 5000–5022, pos vec 4100–4104) | `executeWSE` `0x1401e4dc0`; `0x1400a6c10`–`0x1400ad5a0`; `0x1401e55c5`–`0x1401ec52b`; `0x1401ef0ef`–`0x1401efc8d`; `0x1401f198c`–`0x1401f1c2c`; `0x1401f58a0`–`0x1401fdd80`; `0x140246010`–`0x1402461d7`; `0x14024f4c0`; `0x1402a24f0`–`0x1402b64b0`; `0x14034e340`–`0x1403508a0` |
| `include/wse/vm/lua_{runtime,manager}.h`, `src/lua/` | isolated stock MIT Lua 5.1.5 `WseLuaRuntime` implementing sandboxed `io`/`os`/`package`/`load*` over `Vfs`/`Clock`/`LogSink`, `bit` module, clean-room `LuaGlobals`, `msfiles/` + `variables.txt` parsers, 36 `game.*` + 7 `_G.*` callbacks, op/script/event hooks, and `lua_bridge.h` (`5100`–`5114`) integration | `WSELuaManager` (`0x14033cdd0`, `0x14033d090`; see `lua_runtime.h`, `lua_manager.h`) |
| `include/wse/vm/execute_wse.h` | top-level `execute_wse()` umbrella dispatching across `wse/vm/ops/*.h` | `executeWSE` `0x1401e4dc0` |
| `include/wse/platform/{vfs,saf_vfs,clock,sleep,log,rhi,driver_package,rhi_brf,audio,input,performance}.h` + `../platform/vulkan/driver_loader.{h,cpp}` + `../platform/sdl/{sdl_audio,sdl_input}.{h,cpp}` | **port infrastructure + P6/P7/P8 reconstructions:** `Vfs` (case-insensitive, `/` and `\\` separators, root-confined, optional atomic `rename_file`; `HostVfs` ASCII-folded dir-cache keys with a same-directory check) with `MemoryVfs`, `HostVfs`, and `SafVfs` (`SafTreeBackend`, `MemorySafBackend`, one-time tree index, `import_to`, `scan_warband_modules`, `verify_module_scripts`); `Clock` in the engine's 100,000-per-second ticks with `SteadyClock`/`ManualClock`; `WallClock` (Unix seconds + UTC offset) with `SystemWallClock`/`ManualWallClock`; blocking `sleep_ms`; `LogSink` with stderr, capture, and null sinks; `RhiDevice` + `SoftwareRhiDevice` (`rhi.h`, standard library only), AdrenoTools `meta.json` parser, ELF `.dynstr` `libpanvk_hw.so` patcher, and atomic package staging (`driver_package.h`), BRF/DDS textured mesh helpers + cached `BrfMeshRenderer` (`rhi_brf.h`), `VulkanDriverLoader` (`load_system`, direct-ICD v5 / custom driver preload, `MockVulkanIcd` v5, `VulkanRhiDevice`), `AudioDevice`/`NullAudioDevice`/`CaptureAudioDevice` + `WseAudioMixer` (`audio.h`, stereo 2D/3D mixer + music scheduler) + `SDL_OpenAudioDeviceStream` backend (`sdl_audio.{h,cpp}`), `InputBackend`/`NullInputBackend`/`SyntheticInputBackend`, `WseGameKeyManager`, `WseInputManager` (`InputContext`, virtual stick/look/4-dir attack & block swipes/command radial, opt-in low-pass filtered gyro aim, two-finger `CampaignMap` pinch-to-zoom, `TouchLayoutEditor`, per-module `Vfs` JSON profiles), `FreeCameraController` (`input.h`), SDL3 input/sensor backend (`sdl_input.{h,cpp}`), and `performance.h` (`FrameRateCap` 60/90/120 Hz + uncapped, `PresentModePreference`, hysteretic `ThermalPerformanceController` -> resolution scale / LOD bias / thermal FPS step-down, drift-free `FramePacer`, `RuntimePerformanceHub`, persisted `Vfs` JSON settings) | none for core seam interfaces (replaces Win32 imports in `docs/ARCHITECTURE.md` §6); `WseAudioMixer` reconstructs `0x140290c40`, `0x140290e60`, `0x140290f60`, `0x140331cb0`, `0x140332690`–`0x140333490`; `WseGameKeyManager` reconstructs `0x1402ad360`, `0x1400e9590`–`0x1400ef510`, `0x14069fbd0` (design in `docs/PLATFORM_PLAN.md`) |
| `tests/platform_test.cpp` | host tests for path normalisation and escape rejection, one `Vfs` contract run across `MemoryVfs`, `HostVfs`, and `SafVfs`, `SafVfs` one-time index / `scan_warband_modules` / `verify_module_scripts` / `import_to`, `Vfs` → `TokenReader`, clocks, log sinks, `test_rhi_and_driver_loader` (Turnip/PanVK `meta.json`, `.dynstr` `libpanvk_hw.so` patcher, Mock ICD v5, BRF/DDS textured mesh screenshot & Z-buffer tests), `test_p5_shader_modules_d3d9_and_material_rhi` (`PresentationPath` WSI vs `AHardwareBuffer` bridge, `VkShaderModule` lifecycle, D3D9 projection/half-pixel/sRGB helpers, normal/specular/alpha-test/blend material RHI), `test_p4_review_fixes` (mode ids, name allow-list, nested `meta.json`, atomic re-staging, front face, handle wrap, device generations, mock object accounting), `test_p5_rhi_review_fixes`, and `test_p8_performance_and_frame_pacing` (`FrameRateCap`, `PresentModePreference`, `PerformanceSettings` JSON round-trip & sanitization, `ThermalPerformanceController` headroom/status hysteresis & toggles, `FramePacer` 60/90/120 Hz drift-free cadence over `ManualClock`). The real host Vulkan loader is exercised only with `WSE_TEST_SYSTEM_VULKAN=1` | temp directory + in-memory trees |
| `tests/operand_test.cpp` | host tests for tags, decode, store, slots, tokenizer, statements | local test doubles only |
| `tests/trigger_test.cpp` | host tests for trigger loaders and event-key state machines | local test doubles only |
| `tests/savegame_test.cpp` | host tests for the binary stream and trigger savegames | local test doubles only |
| `tests/script_test.cpp` + `fixtures/scripts.txt` | host tests for script records, header errors, duplicate mapping, and missing-name reporting | local fixture mirroring the retail grammar |
| `tests/wse_test_common.h` + `tests/wse_{bitwise_fp,system,string,dict,array,world}_test.cpp` | host tests for opcode enum values, all implemented handlers (including `*_slot_gt`, scalar/config/skill/savegame MD5, troop HP cap/nibble/proficiency, item/cur_item, edit-mode selection/highlight, `agent_{set,get}_action_speed_modifier` `3340–3341`, party `3900–3911`, `feval`/`parseString`, `.wsedict`/INI/JSON/URL dict I/O, `.wsearray` binary I/O, path sandboxing, natural merge sort, and array relations), operand bounds, and opcode masking | local test doubles only |
| `tests/block_vm_test.cpp` + `tests/native_world_test.cpp` | host tests for analyze/modifiers/loops (live entity iterators `11/12/16/17`, `try_for_dict_keys`), `currentCaller` preservation, default local-variable zeroing (`0x140617819`), core math/scripts/recursion/return values/positions/trigger VM integration/`feval`, `array_sort_custom` script callbacks, `WseWorldState` script integration, unimplemented histogram/`failOnUnimplemented`, Milestone 1 `entity_slots`/`arithmetic`/`positions_and_math`/`strings` suites, Milestone 2 `108` entity/player/quest/UI/site/troop/party + `66` agent/team/prop/mission/timer opcodes, Milestone 3 `284` campaign/mission/item/troop/party/face-key opcodes, and Milestone 4 `218` opcodes + `473/477/1004/1070/1104/1302/1491` non-switch/fallback + `1802/1930/1969` upgrades | local test doubles and module fixtures |
| `tests/module_loaders_test.cpp` | host tests for INI/BOM/comment parsers, individual world/campaign/asset/mission/Data loaders, error diagnostics, full `load_module()` + VM execution round-trips over `MemoryVfs` and `HostVfs`, and reload (module A then B) world-state reset + move-only string storage | in-memory + temp-directory fixtures |
| `tests/headless_test.cpp` | host tests for `run_headless_module()` / `run_headless_session()` (`game_start` + multi-frame triggers over `MemoryVfs` and `HostVfs`, loud unimplemented-opcode failure, compatibility-mode continuation past unimplemented opcodes, and missing `game_start` / load error diagnostics) | in-memory + temp-directory fixtures |
| `tests/lua_test.cpp` | host tests for Lua 5.1.5 stdlib/`bit`/`vector3`/`pos`, `msfiles/` + `variables.txt` parsers, `game.*` / `_G.*` callbacks, trigger/item/prop/prsnt/psys/iterator/hook paths, opcodes `5100`–`5114` + 9 `On*` events, and sandboxed `HostVfs` headless `main.lua` execution | `MemoryVfs` + temp-directory `HostVfs` fixtures |
| `tests/asset_test.cpp` | host tests for all 8 `.brf` resource types (`rfver` 0 and `rfver` 1), `MetaMesh` LOD grouping, `module.ini` resource catalogs over `MemoryVfs`, `.dds` block/uncompressed/`L16`/`DX10` decoding, Warband texture resolution priority, and P5 effect/INI/shader/material resolution + `GLShaders`/WSE2 SPIR-V persistent caching | synthetic binary + `MemoryVfs` fixtures |
| `tests/audio_test.cpp` | P6 review regressions (re-entrant end callbacks & sibling-slot reuse, `set_config` during playback, voice-pitch curve & overflow, whole-sample random start, same-track `play_track`, stream/predecoded parity across chunk boundaries, loop-point interpolation, surround fold, oversized WAV `data`, config sanitising, clock regression, `615` after failure, prop/agent channel replacement, sounds following agents) plus host tests for RIFF/WAVE PCM (`8`/`16`/`24`/`32`-bit & float/`0xFFFE`), OGG Vorbis + MP3 decoding & chunked streaming (`wse_audio`), 4-step sound & music lookup order, `WseAudioMixer` 3D constant-power panning / attenuation / LOD / priority eviction / occlusion hysteresis / deterministic hash / `0.5s`+`5.0s` music scheduler, VM/world audio integration (`599..604`, `609`, `615..616`, `1750`, `1808`, `1881..1882`, `3618`), and opt-in retail `Native` + `Phantasy Calradia` audio gate | synthetic WAV/OGG/MP3 + `MemoryVfs` + `CaptureAudioDevice` (+ optional `HostVfs` retail install) |
| `tests/input_test.cpp` | Slice P7/P8 tests for `get_raw_key_name` (`0x1402ad360` 125 keys), `WseGameKeyManager` (`0x1400e96a0` 75 built-ins + Custom1..20 + modifier chords + gamepad alts + `getKeyData` + `storeKeys`/`restoreKeys`), `InputContext` & gamepad attach overlay hiding, multi-touch stick/look/4-direction attack & block swipes/command radial, `TouchLayoutEditor`, per-module `Vfs` JSON profiles (`%.9g` round-trip & corrupt rejection), `FreeCameraController` driving the P4 `BrfMeshRenderer` viewer, `WorldInputBridge` (`65`, `70..73`, `75`, `77..78`, `3100..3102`), plus P8 opt-in low-pass filtered `Mission` gyro aiming, `CampaignMap` two-finger pinch-to-zoom, and backwards-compatible v1 JSON profile serialization | `SyntheticInputBackend` + `MemoryVfs` + `SoftwareRhiDevice` |
| `tests/wse_selftest.{h,cpp}`, `tests/selftest_host_test.cpp` | shared aggregate runner invoking all `21` core suites (`WSE_SELF_TEST_LIBRARY=1`) for both host verification (`selftest_host_test`) and the Android SDL3 `libwse2.so` app shell (`platform/sdl/sdl_selftest_main.cpp`); `run_all_suites` takes an optional stop callback polled between suites. In the CMake build each suite is compiled once into `wse_selftest` and every `<name>_test` executable is `tests/suite_main.cpp` linked against it | all 21 suites + `fixtures/scripts.txt` |

## Module text format

Module files are whitespace-delimited decimal text. The retail header is three tokens
(`triggersfile version 1`, `simple_triggersfile version 1`, `scriptsfile version 1`, …); the readers
skip bytes `<= 0x20`, take the next token, and parse it. There are no binary records.

Operands are 64-bit words. Values `<= 0` are immediates; otherwise bits 56–62 hold the tag and
bits 0–55 the payload.

| Tag | Meaning |
| ---: | --- |
| `0x00` | immediate |
| `0x01` | global variable array (unchecked in the original) |
| `0x02` | `mbSlots` vector (missing slot reads as 0) |
| `0x11` | local variable (unchecked in the original) |

A statement is `<opcode> <operandCount> <operand>...`. `m_endStatement` is initialized to `-1` by
the constructor and filled by `analyze`.

A trigger is three floats, the conditions block, and the consequences block. A simple trigger is
one float followed by its statement block. Manager records seed every `rglAppTimer` from
`m_defaultTimerNo` and `g_timersTotal[m_defaultTimerNo]`.

Booleans accept `true`/`false` case-insensitively, otherwise a nonzero decimal integer. The retail
float reader uses `strtod`; this port uses `std::from_chars` and rejects trailing garbage. Valid
retail module files parse identically.

## Runtime

`execute_trigger` / `execute_trigger_manager` and their simple-trigger counterparts reproduce the
inlined retail state machines. The `int` argument is an **event key**, not a clock: non-negative
keys run interval triggers from `g_timersTotal`, while negative keys match the rounded negative
check interval (`-112` = agent exit status, `-105`/`-106` = ranged attack, `-26`/`-56` = death, …).
`trigger.h` retains callback-based overloads alongside the state transitions, while
`wse/vm/block_vm.h` connects triggers and scripts directly to the reconstructed
`mbStatementBlock::execute` interpreter (`execute_trigger`, `execute_trigger_manager`,
`execute_simple_trigger`, `execute_simple_trigger_manager`, `execute_script`, `execute_mapped_script`).

## Savegames

`rglBinaryStream` stores raw little-endian values with no header or per-value metadata. Trigger
savegames are an `int32` count, then per manager trigger `int32` status plus `int64` check, delay,
and rearm timer values; simple triggers store one `int64` timer value. The portable
`read_trigger_savegame`/`write_trigger_savegame`/`read_simple_trigger_savegame` use
`SavegameResult`: `count_exceeded` reproduces the retail message-box path while consuming every
entry, and `negative_count`/`truncated` reject malformed input. Retail performs no bounds checks on
binary reads, so truncated files would read past the buffer there.

## Scripts

`scripts.txt` is `scriptsfile version <int32>` followed by the script count and that many
`<id> <interval-seconds> <statement block>` records. `mbScriptManager::readStream` discards both
header strings and the version, replaces the script array, parses every record, then maps the 64
static names. Mapping uses exact string equality and the first script in file order wins, so
duplicate ids resolve to the earliest record. A missing name maps to `-1` and reports through
`ScriptMapCallbacks::reportMissing` (retail logs `"Unable to map script %s"`). Each statement block
id becomes `"Script [<index>] <id>"`.

The portable reader was also run against the retail 3,467,512-byte `scripts.txt` sample: 1,755
scripts, every token consumed, 52 of 64 names mapped.

## WSE extended opcodes

`docs/wse_opcodes.json` and `docs/WSE_OPCODES.md` map every SDK operation in the retail extended range
2800–5202 to the `executeWSE` handler that implements it: 440/440 operations are covered (431 switch
cases and 9 equality-tested cases, with no unknown or unhandled opcode in range). Each entry records the
pseudocode line and the handler's call extent. `tools/extract_wse_opcodes.py` regenerates both files and
`include/wse/vm/wse_opcodes.h` from the 8,844-line WSE2 v1.1.5.5 pseudocode dump and the public
`header_operations_addon.py` names.

`execute_wse` masks the opcode with `0x0FFFFFFF` and dispatches across the handler groups below. The
arithmetic/bitwise group 2800–2809 covers arithmetic right shift (`val_shr`/`store_shr`), logical
right shift (`val_lshr`/`store_lshr`), left shift (`val_shl`/`store_shl`), xor (`val_xor`/`store_xor`),
and complement (`val_not`/`store_not`); shift counts are masked with `& 0x3F`, and `store_*` reads
the value from operand 1 and leaves operand 0 as the destination lvalue.

The register group covers `register_get` (3000), `register_set` (3001), and `store_wse_version` (3002).
The register bank is the 128-entry int64 array at `g_basicGame+0x270` (`0x140662c30`), the same bank
that operand tag 1 addresses; `register_get` writes the indexed value through operand 0, and
`register_set` stores operand 1 into the register named by operand 0 and returns without a store.
Both reject an out-of-range index with `WseDispatch::ignored`, matching `mbCheckRegister`
(`0x1401c4e60`) and the retail `return true` fall-through. `store_wse_version` ignores its value
operand: selector 0 yields the WSE version 5, selector 1 yields the Warband build `0x483` (1155), and
any other selector yields the zeroed default (`0x1401e553a`–`0x1401e555b`).

The script-context, timer, RNG, and profiler group covers `store_current_trigger` (3014),
`return_values` (3015), `store_num_return_values` (3016), `store_return_value` (3017),
`mtsrand`/`mtrand` (3020–3021, `rglMersenneTwister`), `get_time` (3022, UTC/local 32-bit epoch),
`timer_reset`/`timer_get_elapsed_time` (3027–3028, 128 slots tracking 100 kHz `timers_total` with `op1 != 0 ? 0 : 2`),
`set_random_seed` (3034), `store_application_time` (3035), `profiler_{start,stop,is_recording,mark}`
(3040–3043), and `store_cur_mission_template_no` (3600). `store_current_trigger` returns the statement's context
argument only while it is negative (the in-trigger marker; outside a trigger it is 0), matching
`WSEOperationContext::GetCurrentTrigger`. `return_values` writes up to 16 `int64_t` slots at
`0x140698af0`/`0x140698ae8`, `store_num_return_values` reads the count at
`0x140698ae8`, and `store_return_value` reads a 1-based slot (out-of-range/unset logs and returns true without storing).
`store_application_time` is `rglRound64((float)DXUtil_Timer(TIMER_GETAPPTIME) * 1000.0f)` — the
call `FUN_1400b9690(5)` returns QPC seconds since the app-time baseline and the constant at
`0x140576b90` is `1000.0f`. `store_cur_mission_template_no` reads `[0x14061f078]+0x9ae8`, or -1
when there is no current mission.

The dictionary group covers `3200–3222` (`dict_create`..`dict_delete_file_json`, paired with `core_op::kTryForDictKeys` `0x12` in `block_vm.h`): `WSEObjectManager` LIFO handle allocation/reuse, ordered UTF-8 string maps (`rglSectionlessIni`), binary `.wsedict`, `.ini`, JSON (including `\uXXXX` and UTF-16 surrogates), URL-encoded JSON (`+` -> space on decode before unescape; `%2B` and reserved percent-encoding on encode), 9-float `(o, f, u)` transforms with orthonormalization and fallback (`dict_get_pos`/`dict_set_pos`), `dict_load_dict` mode 0 no-op vs loader mode 0 clear-then-load, and `mbCheckPath` filename sandboxing.

The position vector group covers `position_get_vector_to_position` (4100), `position_get_length` (4102),
`get_dot_product_of_positions` (4103), and `get_cross_product_of_positions` (4104) on `rglTransform[128]`
(`positions`).

The string group covers `str_equals` (4200), `str_contains` (4201), `str_starts_with` (4202),
`str_ends_with` (4203), `str_is_alpha` (4204), `str_is_digit` (4205), `str_is_whitespace` (4206),
`str_length` (4207), `str_index_of` (4208), `str_last_index_of` (4209), `str_get_char` (4210),
`str_to_num` (4211), `str_compare` (4212), `str_split` (4213), `str_sort` (4214),
`str_store_lower` (4215), `str_store_upper` (4216), `str_store_trim` (4217),
`str_store_replace` (4218), `str_store_md5` (4219), `str_store_substring` (4220),
`str_store_reverse` (4221), `str_store_join` (4222),
`str_store_replace_spaces_with_underscores` (4223),
`str_store_replace_underscores_with_spaces` (4224), `str_store_float` (4233),
`str_sanitize` (4234), `str_is_integer` (4236), `str_store_server_password_rcon` (4238),
`str_regex_match` (4240), `str_regex_search` (4241), `str_regex_get_matches` (4242),
`str_store_regex_replace` (4243), and `str_decode_url` (4244). Operands resolve through `0x140219c70`: tag 0 (`WseStringBanks::registers`)
is the writable 128-entry string register table at `0x140665030` (assigned by `0x14030fd60` and
copied out by `0x140310970`), tag 0x16 the local string registers (`[0x140621778]`, count
`[0x140621784]`), and tag 3 or an unknown tag the global registers (`[0x140621770]`, count
`[0x140621780]`). `mbCheckStringRegister` (`0x1401c51f0`) validates operand indices and logs on
failure (`WseDispatch::ignored`), while destination tag-0 registers use `mbCheckRegister` (`0..127`).
Comparisons use `wcscmp`/`_wcsicmp`; `str_compare` normalizes the case-sensitive path to `-1/0/1`
(`0x1401f86b0`) and returns the raw folded code-unit difference from `_wcsicmp` (`0x1404989d4`) when
operand 3 is nonzero. `str_contains` lower-cases both strings when operand 2 is nonzero and treats an
empty needle as not contained. The predicates scan the whole string when the index is absent or -1;
an out-of-bounds index logs and leaves the true result. `str_index_of`/`str_last_index_of` take an
optional start (operand 3) and end (operand 4); the last-index handler mirrors the retail loop's
search-before-bound-check order. `str_to_num` uses `_wtof` and truncates after applying the
fixed-point scale when operand 2 is nonzero. `str_split` writes into consecutive tag-0 registers
starting at operand 1 (capped at 127), honors `max` (operand 5) only when more than five operands
are supplied, skips empty pieces when operand 4 is nonzero, and avoids the retail infinite loop on an
empty delimiter by storing the whole source once. `str_sort` sorts a range of tag-0 registers
(case-insensitive on operand 2, reversed copy-back on operand 3). The `str_store_*` handlers write
into tag-0 operand 0: lower/upper case, trim (`0` both, `1` left, `2` right via `iswspace`),
non-overlapping replace-all, lowercase RFC 1321 MD5 (`4219`), `"%.*f"` float formatting (`4233`,
default precision 5), sanitize (`4234`, removes `{`, `}`, `<`, `>`), bounded substring (defaults
`start = 0`, `length = end`; out-of-bounds leaves destination unchanged), UTF-16 code-unit reverse,
range join (with optional valid separator), `" "`/`"_"` conversions, and percent-decoding (`4244`,
matching `UrlUnescapeA(..., 0)`). `str_is_integer` (`4236`) trims both ends and checks `iswdigit`
with an optional leading `'-'`. `str_store_server_password_rcon` (`4238`) is a no-op in v1.1.5.5.
`str_regex_*` (`4240`–`4243`) evaluate ECMAScript `std::regex` over UTF-8 text; invalid patterns
evaluate false or leave stores untouched.

The options flags group (`4300`–`4305`, `0x10cc`–`0x10d1`) exposes `verbose_casualties`,
`cheat_mode`, and `realistic_headshots` via `WseVmState::options`.

The floating-point group covers `fld`, `fld_str`, `fld_pos_x`, `fld_pos_y`, `fld_pos_z`, `fst`, `fcpy`,
`feq`, `fgt`, `flt`, `fge`, `fle`, `fsub`, `fmul`, `fdiv`, `fmin`, `fmax`, `fclamp`, `fadd`, `feval`, and the
transcendental/rounding helpers `fsqrt`, `fabs`, `fceil`, `ffloor`, `fexp`, `fpow`, `fln`, `flog`, `fmod`,
`fsin`, `fcos`, `ftan`, `fsinh`, `fcosh`, `ftanh`, `fasin`, `facos`, `fatan`, and `fatan2` (4400–4438, 100%). Registers are
the 128-entry float array `DAT_140698718` (`g_basicGame+0x35d58`); `mbCheckRegister` (`0x1401c4e60`) accepts indices 0–127.
`fld` divides the fixed-point operand by 100 (`g_basicGame+0x1f8` = `0x140662bb8`, set to `0x64` at
`0x140070ffa` in `mbBasicGame::initialize`); `fld_str` (`4401`) parses operand 1 with `atof`
(`0x1404989cc`);
`fst` multiplies by 100 and rounds with `rglRound64` (`0x1402a1910`): `floorf(x)` plus one when the
fraction is greater than 0.5. Float comparisons use `rglAbs` and `FLT_EPSILON`. `fpow`, `fmod`, and
`fatan2` take operand 1 in `xmm0` and operand 2 in `xmm1`; `flog` reads a literal base from operand 2
(default 10, minimum 2). `feval` (`4437`) runs `mbBasicGame::parseString` (`0x1400720b0`, turning literal
`'^'` into `'\n'` while leaving expanded `{sN}` contents un-reparsed) and compiles the full expression
to an AST (`mu::Parser` semantics) before executing so syntax errors leave `fp` and `positions` intact.

The dynamic array group (`5000`–`5022`, `array_create`..`array_le`) shares `WseObjectManager` LIFO object
IDs and `mbCheckPath` sandboxing with dictionaries across 1–14D integer, string, and `rglTransform` arrays,
`.wsearray` binary save/load (`[o.xyz, f.xyz, u.xyz]` transform serialization with `orthonormalize`),
bottom-up natural merge sort (`array_sort` and `array_sort_custom` invoking `execute_script` via
`wse_invoke_script_callback`), and lexicographical comparisons (`array_eq`..`array_le`). Handlers that do
not write operand 0 return `WseDispatch::handled_no_store`;
a failed comparison returns `WseDispatch::condition_false`; an invalid register is a successful no-op
(`WseDispatch::ignored`), matching the retail `return true` fall-through.

Every mapped opcode without a transcribed handler returns `WseDispatch::unimplemented`. The
implemented/remaining tally per opcode family is in `STATUS.md` §3.

Native (non-WSE) opcodes report the same way: an unimplemented native opcode yields
`NativeDispatch::unimplemented` and is recorded in `VmContext::unimplementedCount` and the per-opcode
`unimplementedOpcodeCounts` histogram. In compatibility mode the statement still evaluates `true`;
with `failOnUnimplemented` the block aborts with an error (`ROADMAP.md` backlog B6).

## Safety deviations

The retail code trusts operand/statement/script/trigger counts and timer indices. This port rejects
negative counts, operands above 32, oversized arrays, out-of-range timer indices, and truncated
binary savegames; none of these checks changes valid module behaviour. In `str_split` (4213), an
empty delimiter cannot advance in retail and would loop indefinitely; the port stores the source as
a single piece (respecting `skip_empty`) and terminates.

Asset decoders (`wse/asset/{brf,dds}.h`, P3 review & evidence follow-ups):

- `.brf` element counts are checked against the remaining stream bytes at each record's minimum
  wire size, and at most `detail::kMaxReserveElements` (`4096`) elements are reserved before they
  are read, so a corrupt count cannot allocate many times the file size. Count errors keep their
  specific message (`truncated … count`, `negative count`, `count N exceeds remaining stream bytes`).
- A face-corner vertex index above `65535` is rejected: retail `rglMeshFaceCorner::m_vertexNo`
  (`+0x40`) is an `unsigned short`, so such an index would be truncated.
- Composite bodies nest at most 8 levels.
- `dwDepth` is read only for volume textures (`DDSD_DEPTH`, `DDSCAPS2_VOLUME`, or DX10
  `TEXTURE3D`); other files are 2D/cube images whatever `dwDepth` holds. A volume with
  `dwDepth > 1` is rejected rather than decoded as its first slice; a single-slice volume
  (`dwDepth` `0` or `1`, e.g. retail Warband `Textures\languages\cnt\font.dds`) is accepted as a
  2D image. DX10 `array_size` is capped at 64. A DX10 `TEXTURE3D` header with `array_size > 1` or
  the `TEXTURECUBE` misc flag is malformed (D3D10/11 forbid both) and rejected as a volume texture.
  DX10 cube maps count `6 × array_size` faces; legacy cube maps count their `dwCaps2` face bits.
  `DdsImageView::is_srgb` marks the DXGI `*_SRGB` formats, whose `format` is the UNORM equivalent.
- `decode_dds_mip_rgba8` re-validates the slice index and that the slice payload covers its
  dimensions. `DdsImageView` slices are non-owning views into the buffer given to `parse_dds`.
  Every texel of the output is written, so a caller's buffer is resized but not cleared first
  (reuse one buffer across decodes); BC blocks are stored a 4-texel row at a time and only
  right/bottom edge blocks of a non-multiple-of-4 image are clipped.
- Verified against the v1.1.5.5 control flow: `rglResourceManager::createMetaMesh` (`0x1402f29a0`)
  requires `prefix.length >= 2` (`0x1402f2b23`), splits at the first `.`, and routes dotted non-LOD
  names to LOD `0`; `rglResourceManager::tryGetTexture` (`0x1402f2f10`) + `rglTexture::initDeviceObjects`
  (`0x140317710`) probe module language, module `Textures`, `TexturesHD` (single-frame only, before
  base `Textures\languages`), base `Textures\languages`, and base `Textures`, with indexed
  `%s_%d.dds` frames (`((flags >> 24) & 0xF) * 4`) skipping `TexturesHD` and single-frame
  extensionless names falling back to exact / `.jpg` / `.tga` (callers must check extension before
  `parse_dds`).
- `resolve_texture_path` with an empty `warband_root` probes the base directories at the VFS root
  (`vfs_join_path("", x) == x`), as `load_module_brf_catalog` does for `CommonRes`.
- `resolve_texture_path` skips a directory that `is_dir` reports missing instead of probing each
  candidate in it (port-side optimisation; the retail probe order and results are unchanged).
- `HostVfs` matches a component exactly first, then against a cached lower-case index of the
  parent directory. The index is rebuilt when the directory's last-write time changes and dropped
  on every `write_file` / `remove_file` / `make_dir` / `remove_dir`; among entries differing only
  in case the byte-wise smallest name wins. A differently-cased file added by another process
  inside the file system's timestamp resolution can stay unseen until the directory changes again.

Script operands never size an allocation directly (Milestone 4–6 review fixes):

- The Lua bridge stack is capped at `WseLuaState::kMaxLuaStackSize` (`8000`): `lua_set_top` (5101)
  beyond it is ignored and pushes past it are dropped with an error. The bridge error log keeps the
  newest `kMaxLuaErrors` (`256`) entries.
- The Lua 5.1.5 runtime (`engine/src/lua/`) removes `os.execute`/`os.getenv`/`os.exit`/`os.tmpname`/`os.setlocale`,
  `package.loadlib`, and `io.popen`; replaces `io.*` and `os.remove`/`os.rename`/`load*`/`dofile`/`require`
  with `Vfs`-confined equivalents (`%storage%` / `%savegames%` tokens, no upward writes outside
  `<module>/lua/`), and rejects precompiled `\x1b` bytecode chunks.
- Lua runtime safety deviations (`engine/src/lua/`, regression-tested in `lua_test.cpp`):
  - Stock Lua 5.1.5 is compiled as C++, so a Lua error raised inside a `game.*` / `io.*` callback
    unwinds as an exception and runs C++ destructors instead of `longjmp`-ing over them.
  - Every C function registered with Lua goes through `guarded<>` (`wse_lua_internal.hpp`): a C++
    exception escaping a callback (`std::bad_alloc`, a throwing `Vfs`) is re-raised as the Lua error
    `C++ exception in Lua callback: <what>`. Without it Lua's `catch (...)` reports status `-1`
    with no message.
  - VM-facing entry points (`lua_push_*` `5108`–`5110`, `lua_set_top` `5101`, event and hook calls)
    reserve Lua stack space first; a push or `lua_set_top` that cannot be satisfied is dropped with a
    `Lua stack overflow` error instead of writing past the stack.
  - Values read outside a protected call (`lua_to_int` `5105`, operation-hook lhs results,
    script-hook replacement parameters) use a non-raising conversion: inf/NaN/out-of-range numbers
    read as `0` or are skipped with an error rather than terminating the process.
  - `lua_triggerCallback` (`5113`) always removes its `num_args` arguments and its result from the
    stack, including when the reference is not a function.
  - While any trigger is executing (`VmContext::triggerIterationDepth > 0`), `game.addTrigger`,
    `removeTrigger`, the item/scene-prop trigger variants, and `addPrsnt` / `removePrsnt` are queued
    and applied in call order once the outermost trigger execution unwinds (retail edits the arrays
    in place). Returned indices, booleans, and `getNum*Triggers` counts already reflect the queued
    edits (including `removePrsnt` by id); a trigger that removes itself finishes its current run.
    Two visible differences from in-place editing: a trigger removed mid-pass still fires if it sits
    later in the manager being walked, and a presentation added mid-trigger cannot be addressed by
    index from VM opcodes until the trigger unwinds. `TriggerIterationScope` applies the queue when
    the outermost scope ends, so code that walks `ModuleBundle::presentations` (or another
    container the edits resize) and runs a trigger manager per element must hold its own scope
    around the walk. A queued edit that throws while being applied is dropped and reported through
    `VmContext::report_error`.
  - Sandboxed `io` follows stock 5.1 results: `read("*a")` at end of file returns `""`, `file:write`
    returns `true`, `io.close()` closes the default output. `os.rename` onto the same
    (case-insensitive) path is a no-op instead of deleting the file.
  - Files Lua leaves open are flushed by an explicit `WseLuaRuntime::shutdown()`; destroying the
    runtime without it discards their unflushed data, because the `Vfs` may already be gone.
    `run_headless_module` shuts down any previous Lua state before loading a module; that shutdown
    (and the one at the start of `initialize()`) uses the previous run's `Vfs`, bundle, and
    `VmContext`, which must still be alive unless `shutdown()` was already called.
  - `msfiles/*.py` constant expressions whose operands do not fit 64 bits, or that shift by a count
    outside `0..63`, are skipped; values outside the opcode table are not registered as operations.
- `spawn_scene_prop` (1974) stops at `kMaxSpawnedMissionObjects` like `spawn_item`; `spawn_horse`
  (1973) stops at `kMaxSpawnedAgents` and leaves `reg0 = -1`.
- `overlay_set_boundaries` (928) and `scene_set_height_fog` (4518) convert float to `int32` with
  saturation, and the `send_message_to_url_advanced` (3019/3033) seconds-to-milliseconds timeout
  saturates instead of overflowing.

Module scripts can mutate an object while the VM is using it, so the VM never keeps a pointer into
one across script-visible execution:

- `try_for_dict_keys` (`0x12`) stores the dictionary id and the current key in its loop frame and
  re-resolves both on every step (`upper_bound` of the last key). A body that frees or clears the
  dictionary ends the loop; erasing the current key or inserting keys is safe, and keys inserted
  after the current one are visited.
- `array_sort_custom` (5016) sorts a detached copy and installs the result only if the id still
  names an array of the same type and shape. A comparator that frees, pops, or resizes the array
  therefore leaves its own edits in place and the sort result is dropped; element edits made by
  the comparator to a same-shaped array are overwritten by the sorted copy.
- Dynamic arrays are limited to `kWseArrayMaxElements` (2^24) leaf elements: `array_create` and
  `array_load_file` store 0 and `array_resize_dim` is a no-op beyond that, instead of passing an
  arbitrary dimension product to the allocator.
- The statement, script, and trigger loaders allocate records as they are read and reserve only
  what the unread input could hold, so a corrupt count cannot reserve hundreds of megabytes.
- Control opcodes (`try_begin`, `try_end`, `else_try`, `break_loop`, `continue_loop`) are matched
  after masking with `0x0FFFFFFF` in the executor, the same way `analyze` matches them. This is a
  consistency fix, not an evidence-backed retail fact: the compiler never emits modifier bits on
  these opcodes, and the retail handling of that case has not been checked.
- `convert_to_fixed_point` (2130) wraps on overflow; `convert_from_fixed_point` (2131) reports
  `"Division by zero"` if a host zeroes `fixedPointMultiplier` (scripts cannot set it below 1).
- `HostVfs` resolves symbolic links and refuses any path whose target lies outside its root.
- Party and troop counters saturate instead of wrapping: `troop_add_xp` clamps `WseTroop::xp` at
  `INT32_MAX`, and `party_add_members`, `party_add_leader`, and `WseParty::add_members` clamp
  `num_troops` the same way (`saturating_add_i32`). `party_add_prisoners` (1611) then stores the
  delta that was really applied in `reg0`.
- `party_add_members` and `party_add_leader` reject an invalid troop id, so
  `party_add_prisoners` (1611) and `party_add_leader` (1612) can no longer create a stack whose
  `troop_no` is out of range. Whether retail checks this has not been verified.
- `add_visitors_to_current_scene` (1265) stops at `kMaxSiteVisitors` (4096) entries per site.
- `get_sq_distance_between_positions` (712/713) and `add_gold_as_xp` (1063) convert their float
  results with saturation (`NaN` gives 0) instead of an out-of-range float-to-integer cast.
- Milestone 4–6 review fixes (portable-model decisions; none of these has been checked against
  the retail handlers beyond the public operation semantics):
  - `leave_encounter` (1301) sets `misc.encounter_left`; `misc.save_mode_flags` is read only by the
    difficulty formula. `set_merchandise_modifier_quality` (1490) stores a float fraction.
  - `spawn_item` (1971/1976) sets both `is_item` and `m5.is_spawned_item`, so
    `scene_spawned_item_*` (1832/1833) see spawned items and `scene_item_*` (1830/1831) see only
    scene-placed ones. `spawn_scene_prop` (1974) uses `world.spawn_position`.
  - `agent_set_scripted_destination_no_attack` (1748) works on a copy of the position register.
    `agent_deliver_damage_to_agent_advanced` (1827) treats the weapon item as optional.
  - `agent_get_wielded_item_slot_no` (3308) returns the equipment slot `0..3` holding the wielded
    item kind, or `-1`. `agent_{get,set}_current_vertical_speed` (3331/3332) use `speed.z`.
    `agent_set_horse` (3313) validates the new horse before detaching the old one.
  - `missile_remove_on_hit` (3611) flags only `m6.trigger_missile_no`, which the mission runtime
    must set while `ti_on_missile_hit` runs. Missile positions (3609/3613) use world up `(0,0,1)`.
  - `skin_set_blood_color` (5200) stores one colour per skin (`WseM6SkinExt::blood_color(skin)`).

Platform P4 (`wse/platform/{rhi,driver_package,rhi_brf}.h`, `platform/vulkan/driver_loader.{h,cpp}`;
port infrastructure):
  - `patch_icd_libhardware_needed` parses the ELF (little-endian ELF32/ELF64, every offset bounds-
    checked) and rewrites `libhardware.so` -> `libpanvk_hw.so` only in strings reached through
    `PT_DYNAMIC` -> `DT_STRTAB` (`DT_NEEDED` and `DT_VERNEED` `vn_file`); identical text in
    `.rodata` is not touched. `stage_driver_package_to_vfs` rejects a `.so` payload that is not ELF.
  - `SoftwareRhiDevice` clips against Vulkan's near plane (`z >= 0`), a minimum `w`, and an 8x
    guard band before rasterizing, so triangles crossing the camera are cut instead of dropped and
    huge or NaN coordinates never reach an integer conversion. Positions snap to 1/256 pixel and
    edge functions are exact `int64`, with the top-left fill rule: pixels on a shared edge are drawn
    exactly once. Every varying (UV and colour) is perspective-correct. Vertices are transformed
    once per draw and buffers are read in place (no per-draw copies).
  - `VulkanDriverLoader`: custom drivers are opened `RTLD_NODELETE` (Mesa threads / exit handlers);
    entry order `vk_icdGetInstanceProcAddr` -> Android `HMI` hwvulkan module -> `vkGetInstanceProcAddr`;
    instance `apiVersion` = `min(vkEnumerateInstanceVersion, 1.3)` with a 1.0 retry on
    `VK_ERROR_INCOMPATIBLE_DRIVER`; `textureCompressionBC` is enabled when present and reported;
    textures above `maxImageDimension2D` are rejected; `vkDeviceWaitIdle` runs before teardown;
    moved-from loaders report an error instead of dereferencing null; zero-sized buffers are
    rejected (Vulkan forbids them).
  - `run_vulkan_mesh_smoke_test` reports a mock fallback through `fell_back_to_mock` /
    `fallback_reason` (never as a plain success); the Android probe disables the fallback unless
    the launcher's testing checkbox is ticked. Its frame comes from the CPU reference rasterizer.
  - Second review (2026-11-15):
    - `parse_driver_mode` accepts only the stable ids (`system`, `turnip`, `panvk`, `custom_icd`,
      plus exact aliases `freedreno` / `panfrost` / `custom`), case-insensitive. Substring matching
      once read "not turnip" as Turnip. The launcher stores and passes ids, not display labels
      (old label preferences are migrated).
    - `is_safe_driver_library_name` is an allow-list: 1..128 characters from `[A-Za-z0-9._+-]`, no
      leading `.` or `-`, no `..`, a `.so`/`.dll` suffix with a non-empty stem.
    - `parse_driver_meta_json` skips nested array/object members instead of rejecting the file
      (`wse_parse_json_object(..., skip_nested = true)`; the WSE dict ops keep the strict default).
      `check_driver_package_compat` refuses a package whose `minApi` exceeds the device API level
      or whose kind (Turnip/PanVK) differs from the selected one; the JNI staging call applies it.
    - `stage_driver_package_to_vfs` patches the ICD in place (rvalue overload; the span overload
      copies once), creates the stage directory or fails with an error, and writes the ICD first
      and `meta.json` last, each through `<name>.partial` + `Vfs::rename_file`. An existing file is
      never truncated in place, so a still-mapped driver (`RTLD_NODELETE`) keeps its pages and an
      interrupted write leaves the old package whole. Without rename support: remove + write.
    - `SoftwareRhiDevice`: `PipelineDesc::front_face` (default `counter_clockwise`, Vulkan
      framebuffer terms) decides which winding culling keeps; a failed pass leaves `out_rgba8`
      empty; `clear_depth` outside `[0, 1]` or NaN is rejected (NaN disabled depth testing); after
      the 32-bit handle counter wraps, allocation skips every live handle.
    - `make_orbit_mvp_matrix`: far plane `max(100, 4 * distance)`, near `max(0.1, distance / 1000)`;
      non-finite arguments fall back to defaults.
    - `VulkanDriverLoader::device_generation()` (process-unique per `VkDevice`); `destroy_vk_buffer`
      / `destroy_vk_image` take the generation recorded at creation and ignore stale handles, whose
      values the next device may reuse (a stale destroy used to free another owner's buffer).
      Negotiated ICD interface versions outside `1..5` are refused. On Windows the system loader is
      searched in System32 only (`LOAD_LIBRARY_SEARCH_SYSTEM32`) and custom paths are UTF-8
      (`LoadLibraryExW`).
    - The mock ICD counts live objects (`mock_vulkan_object_stats`): children still alive at
      `vkDestroyDevice` and unknown/double destroys are reported.
    - Android: the probe runs in `WseVulkanProbeService` (`:vkprobe` process, one probe per
      process, 60 s timeout); a crashing or hanging driver ends only that process.
    - `libpanvk_hw.so` is built from `platform/android/panvk_hw_stub.c` (active copy of the legacy
      `runtime/android/src/hardware_stub.c`).

Platform P5 (`wse/asset/shader.h`, `wse/platform/rhi.h`, `platform/vulkan/driver_loader.{h,cpp}`):
  - `VulkanDriverLoader::probe_and_create_device` queries `vkEnumerateInstanceExtensionProperties`
    and records `PresentationPath`: `android_surface_wsi` (`VK_KHR_surface` + `VK_KHR_android_surface`)
    when entered through the Android system loader (`!used_direct_icd`), vs.
    `offscreen_hardware_buffer_bridge` (`AHardwareBuffer` + `ASurfaceControl`) for direct custom
    Turnip/PanVK `.so` `dlopen`.
  - `VulkanRhiDevice::{create,destroy}_shader_module` validates SPIR-V headers and manages real
    `VkShaderModule` handles via `vkCreateShaderModule` / `vkDestroyShaderModule`.
  - `SoftwareRhiDevice` keeps a fast path preserving P4 frame hashes bit-for-bit, and enables
    D3D9 RH perspective/ortho depth `[0, 1]`, `(+0.5, +0.5)` half-pixel compensation, IEC 61966-2-1
    sRGB decode/encode, Blinn-Phong diffuse+specular with tangent-space normal/specular maps,
    alpha-test discard before depth write, and the 5 D3D9 blend modes (`opaque`, `alpha`,
    `additive_alpha`, `multiply`, `additive`).
  - `wse/asset/{shader,shader_compiler}.h` + `src/shader/wse_shader_compiler.cpp` (`wse_shader`) model
    effect search order, `wse2_shaders.{brf,ini}`, cycle-guarded `rglShader` suitability/alternative
    fallback, `rglMaterial` state/slots, `GLShaders/*.xml` + `GLShadersOptimized` + the 5 synthesized
    WSE2 skinning variants, Warband GLSL → Vulkan `#version 450` lowering + Khronos `glslang 16.6.0`
    SPIR-V 1.0 compilation, and persistent VFS `SpirvShaderCache` (`kWseSpirvGeneratorVersion = 4`).

Platform P5 review fixes (behaviour notes and safety deviations):
  - **SPIR-V 1.0 compiler backend:** `compile_warband_glsl_to_spirv` runs glslang's preprocessor on
    the legacy source (after splicing `\` line continuations, which glslang rejects below 4.20), then
    `lower_warband_glsl_to_vulkan450` rewrites it to `#version 450` and `create_glslang_shader_compiler()`
    compiles it to SPIR-V 1.0 tagged by an `OpString` (`wse2:tech=…:be=<glslang version>:decl=…:feat=…:
    src=…:gen=4`; `inspect_spirv_module` rejects placeholder `WSE2` modules, truncation, and mismatched
    stage/decl/source). Compiles may run concurrently on one backend.
  - **Interface layout (port choice, no retail counterpart):** pass the technique's other stage as
    `partner_glsl`. Varying locations and sampler bindings are assigned from the union of both
    stages' declarations in sorted-name order: names in the fixed Warband tables keep their slot
    (`attribute` `0..8`, `varying` `0..8`, samplers `2..13`) unless an earlier name already holds
    it; everything else gets the first free run of slots (varyings from `9`, samplers from `14`,
    attributes from `9`), sized by matrix columns and array length. Uniform blocks: binding `0`
    VS / `1` FS, `std140`, members `_wse_u_<name>`; `bool` stays `bool` (glslang lays it out as a
    32-bit value).
  - **Rewriting:** loose uniforms → block members, except where a local, parameter, `for` variable,
    or struct member shadows the name; `#define` bodies get the same renaming (macro parameters
    excluded) when the source is not preprocessed. Top-level `struct` definitions are emitted
    before the uniform block. `gl_FragColor` → location-0 output; `gl_FragData[n]` → a location-0
    output array sized by the highest literal index (`4` for a computed index); writing both is
    an error. `texture{1D,2D,3D,Cube}{,Proj,Lod,ProjLod}` and the `*ARB` lod/grad forms →
    `texture*`; `shadow2D{,Proj,Lod,ProjLod}` → `_wse_shadow2D*` helpers returning `(r, r, r, 1)`
    (the GLSL 1.20 `vec4` result in `GL_LUMINANCE` depth mode). `invariant` is kept on vertex
    outputs only; `centroid` / `flat` / `smooth` / `noperspective` are kept.
  - **Uniform initializers:** `uniform T x = v;` becomes a plain member; `v` is reported in
    `ShaderStageReflection::uniform_defaults` for the uploader to write.
  - **Reflection:** member names are the Warband uniform names; `size_bytes` is the `std140`
    footprint (`array_stride_bytes * array_size` for arrays, `matrix_stride_bytes * columns` for
    matrices), not glslang's element count.
  - **Cache:** files are keyed by stage, vertex declaration, and the source of both stages, not the
    technique name, so techniques that share GLSL share a module. `<hash>.refl` stores the
    reflection next to `<hash>.spv`; a hit that needs reflection but has no valid sidecar is a
    miss. With a backend set, a module tagged by another backend identity (e.g. an older glslang)
    is recompiled; with no backend, any identity is accepted.
  - **Technique XML:** `<technique` must be followed by a tag boundary (a `<techniques>` root no
    longer swallows the first technique); comments, CDATA, `<?…?>`, and `>` inside quoted values are
    skipped; `<vertex_shader name="…"/>` and `<vertex_shader>…</vertex_shader>` are both read.
  - **Technique catalog:** fails (`false` + error) when no technique exists at all; the 5 WSE2
    skinning variants are synthesized only from a present base, with a real `#define USE_SKINNING 1`
    after `#version`. `TechniqueCatalogOptions::load_glsl_sources = false` loads names only.
  - **Shader resolution:** breadth-first with one visited set, `O(shaders + alternatives)` (the
    path-local DFS was exponential on dense graphs); ties go to alternative-list order.
    `SuitableShaderResolver` memoizes per case-folded name.
  - **Feature flags:** whole-identifier, case-insensitive matches outside comments; preprocessor
    lines count only `#define NAME`. Allocation-free.
  - **`wse2_shaders.ini`:** a UTF-8 BOM is skipped; a key repeated in one section keeps its first
    position but the later value everywhere, with a warning.
  - **PostFx (safety deviation):** the WSE2 include is appended unless a `#if`/`#ifdef`/`#elif`
    directive tests `WSE2` as a whole token (incl. `defined(WSE2)`), instead of a substring test.
  - **Material state:** `uses_specular` is gated by `shader->m_flags & kShaderFlagHasSpecular`
    (`0x20`, `0x1402e8454`) when a shader is resolved (and uses the material's specular
    map/color/coeff rule when unshaded); `alpha_ref_u8` (`0x1402e8439`/`0x1402a00b0`) stays
    active even on blended materials; `render_order` signed nibble (`-8..+7`) matches
    `rglMesh::updateRenderOrder` (`0x1402b9ef5`); blend `7` (`0x700`) is per-draw vertex alpha
    (`0x1402e913f`); DX9 sets no sRGB texture/write states (`rglMesh::gammaCorrect` `0x1402c02b0`
    and shaders handle gamma), so `srgb_diffuse = !diffuse_a.empty()` is used for Vulkan sRGB.
  - **SPIR-V cache:** a hit requires the whole module to validate and its tag to match the source
    hash, stage, and generator version (part of the key); writes go through `.partial` +
    `Vfs::rename_file`. Hits return feature flags from the tag.
  - **RHI:** shader modules can be destroyed after pipeline creation (`VkShaderModule` semantics);
    `create_shader_module` requires an `OpEntryPoint` matching stage and name, and pipelines reject
    a module of the wrong stage. `VulkanRhiDevice` fails module creation when the device lacks
    `vkCreateShaderModule`. `MeshDrawCall::model` transforms normals (inverse transpose) and
    per-triangle UV tangents for lighting; alpha blends with the colour factors (D3D9 without
    `SEPARATEALPHABLENDENABLE`); NaN-safe UNORM writes; exact 256-entry sRGB decode table; the
    destination is read only when blending. `apply_d3d9_half_pixel_offset_clip` takes `w` and the
    NDC Y convention. `wse/platform/rhi_material.h` maps `MaterialRenderState` to `PipelineDesc`.
  - **WSI:** `android_surface_wsi` now enables `VK_KHR_surface` + `VK_KHR_android_surface` on the
    instance and `VK_KHR_swapchain` on the device; a device without it (or an instance refusing the
    extensions) falls back to the offscreen bridge with `presentation_note`.

P6 audio (`wse/asset/audio.h`, `wse/platform/audio.h`, `wse/game/world_audio.h`, P6 review fixes):

- **WAV:** a `data` chunk whose declared size runs past the end of the file (`0xFFFFFFFF` from
  streamed writers, or a cut-short file) plays the frames present instead of failing; other
  truncated chunks are still rejected. Memory decodes stop at `kMaxDecodedAudioSamples` (10 minutes
  of 48 kHz stereo); music always streams (`WavStreamDecoder` converts from the file bytes per
  read, never the whole file).
- **Surround fold:** sources with 3–8 channels fold to stereo by speaker position (WAV
  `dwChannelMask` or its default, Vorbis I channel order): front 1.0, centre/side/back -3 dB,
  back-centre and unknown -6 dB to both sides, LFE dropped, normalised so the fold cannot clip.
  FMOD's own downmix is not reconstructed. MPEG sync detection rejects the reserved layer `00`
  (AAC ADTS).
- **Mixer:** channel-end callbacks are queued and delivered once the mixer is consistent (after the
  mix block, after the stop, or after the new sound owns an evicted slot), so callbacks may play or
  stop sounds; starting a new sound into a previously inactive sibling slot mid-flush cancels any
  still-queued stale end notification for that slot. **Deviation:** a sound started from inside an
  end callback never evicts (it gets a free slot or `-1`); otherwise the evicted sound's queued end,
  which carries only a channel number, would be delivered after the caller recorded that channel as
  its own. `set_config` sanitises values (`max_distance_3d >= min_distance_3d > 0`, finite
  non-negative volumes, channel counts in `[1, 256]`) and only ends channels when
  `max_virtual_channels` shrinks. Per-voice and per-music-slot gains ramp linearly across each
  block; the bus uses `soft_clip_sample` (identity to 0.8, tanh knee) instead of a hard clamp; the
  occlusion low-pass coefficient scales with the output rate and is primed from the current sample
  when it engages. Looping samples interpolate across the loop point; streamed music carries the
  last frame of each 2048-frame chunk so interpolation is continuous across refills.
- **Random start (`kSoundFlagRandomStartPos`):** `(rand15 * frames) >> 15`, covering the whole
  sample (intentional improvement over retail `0x140332d80` `rand() % m_length`, which only reached
  the first 32767 frames).
- **Music scheduler:** `play_track` for the track already playing (option 1 or 2) keeps it playing
  and cancels a pending fade instead of stopping it and then dropping the request. `update()`
  re-bases its stored timestamps when the caller's clock jumps backwards. Clocks are `double`.
- **`last_sound_channel` / `615`:** the mixer reports the result of the most recent play request,
  `-1` included, matching what `599`/`600`/`1750`/`1881` store.
- **Opcodes:** `1881` stops the prop's previous channel before playing (one sound per prop
  instance); `1750` stops the agent's previous channel only when it is looping (one-shots finish)
  and passes `compute_agent_voice_pitch(agent_no, troop_no, party_no)` (`0x140057010` `m_no`,
  `m_troopNo`, `m_partyNo`; `0.3f` scale, `1.0f / (1.0f - delta)` for negative `delta`, player agent
  kept at `1.0f`) for human agents. `WorldAudioBridge::update` moves agent/prop channels to their
  owners' positions each tick. A sample that fails to read or decode is remembered and not retried
  on every play.
- **Not implemented / verified flags:** Doppler (`velocity`), HRTF (`FMOD_INIT_HRTF_LOWPASS` `0x10`
  at `0x140332f80`). `0x140332d80` has no random frequency jitter (`kSoundFlagDisableFreqVariation`
  `0x8` gates caller pitch multiplier and streaming); `distance_filter` gates 3D distance rolloff
  instead of FMOD's `FMOD_INIT_DISTANCE_FILTERING` (`0x200`) distance low-pass.

P7 input (`wse/platform/input.h`, `wse/game/world_input.h`, `platform/sdl/sdl_input.{h,cpp}`):

- **`WseGameKeyManager` & raw keys:** exact 125-entry `rglInputManager::getKeyName` (`0x1402ad360`)
  table and 95-slot `mbGameKeyManager` (`0x14069fbd0`, `0x1400e96a0`) defaults (`75` built-in
  actions `MoveForward`..`TakeScreenshot` + `Custom1..20`, `Left Ctrl` modifiers, `13` gamepad
  alt bindings `0xf0..0xff`), primary+alt `keyDown`/`keyClicked`/`keyRaised` (`0x1400eedf0`..
  `0x1400eef50`), `getKeyData` (`0x1400ef190`), and `3100` `game_key_get_key` (`0x1401e6a42`:
  `[key, mod0, mod1]` indices `0..2`).
- **Edges (`WseInputManager`):** raw clicks/releases are latched when the transition happens, so a
  key pressed and released inside one frame still clicks (and releases), and every wheel notch is
  a click. Physical game-key clicks/releases use `keyClicked`/`keyRaised` semantics (modifiers held,
  main key changes state); overlay- and gamepad-synthesized keys and one-frame pulses (radial
  orders, same-frame attack/block taps) add their own edges. A release is only reported once the
  game key is no longer held by any source. `WorldInputBridge` writes `prev_key_down` /
  `prev_game_key_down` so `3101`/`3102` see exactly these release edges.
- **`omit_key_once` (`77`):** the next fresh press of the key is masked out of game-key mapping
  until it is released, and the request is consumed (also in `world.input.omitted_keys`, so it
  expires for scripts). `70`/`71` still return false while it is pending (see `STATUS.md` §5).
- **Safety / robustness deviations:** `FocusLost` (window focus lost, minimized, or app in the
  background) releases every key, button, and touch; `TouchCancel` (SDL `FINGER_CANCELED`) drops
  the finger without issuing the highlighted radial order; a removed gamepad releases the buttons it
  held; per-pad state is kept and the most-deflected stick wins. Android Back maps to `Esc`;
  "natural" wheel direction is undone; touches synthesized from a mouse are ignored.
- **Analog movement:** `move_x/move_y` = touch stick + gamepad stick (+1 per *physical* move key),
  limited to unit length. Keys synthesized past `stick_key_threshold` feed `game_key_down` only, so
  the stick stays proportional. The gamepad stick also drives `MapMove*` on the campaign map.
- **Cursor (`75`):** `world.m5.cursor_pos` is in Warband UI space: x `0..1`, y `0..0.75` growing
  upwards (`y = (1 - screen_y) * 0.75`), the space `751` already assumed.
- **Context-sensitive touch overlay & layout profiles:** `InputContext` (`Mission`, `CampaignMap`,
  `Menu`, `Inventory`, `Presentation`, `Dialog`) suppresses combat zones in `CampaignMap` and hides
  the overlay (cancelling active touch latches) in non-gameplay contexts or while a connected
  gamepad is the device in use; touching the screen shows it again until the gamepad is used.
  Geometry is aspect-correct (`set_viewport_size`): circular widgets are circles on screen,
  swipe/radial classification and look deltas use screen-height units. Mouse motion turns the
  camera only in `Mission`/`CampaignMap` with `set_mouse_look_enabled(true)`.
  Multi-touch latches each finger by hit priority (`MoveStick`, `CommandRadial`, `AttackZone`,
  `BlockZone`, fallback `LookArea`), classifies 4-direction attack/block swipes, maps radial release
  to `Order1..8`, clamps `TouchLayoutEditor` edits to safe screen/scale bounds, and persists
  sanitized per-module JSON profiles via `Vfs` with locale-independent `to_chars` floats. Profile
  file names (`input_profiles/<id>.json`) fold ASCII case and `%XX`-encode every other byte except
  `[a-z0-9_-]` and inner `.`, so punctuation and non-Latin module names get their own file; names
  with `/`, `\`, control bytes, or `..` are refused. Binding names are re-copied into the world only
  when `WseGameKeyManager::revision()` changes. `FreeCameraController` matches
  `make_orbit_mvp_matrix` orientation and wraps yaw.

## Handler design decisions and limits

- **Exceptions (`execute_wse` / `DispatchFn`):** `execute_wse()` and all eight `execute_wse_*`
  family dispatchers are **not** `noexcept`. Handlers perform dynamic allocation, VFS/stream I/O,
  and `std::regex` evaluation; while regex syntax/execution errors are caught at the opcode sites,
  removing `noexcept` lets genuine `std::bad_alloc` or `std::filesystem::filesystem_error` (inside `HostVfs`)
  failures unwind instead of calling `std::terminate` across platforms.
- **Character classes and case folding (`str_is_alpha`, `str_is_digit`, `str_is_whitespace`, trims, case folds):**
  `wse/core/encoding.h`, `wse/vm/wse_state.h`, `wse/vm/ops/string.h`, and `wse/vm/ops/fp_expr.h`
  avoid `<cctype>`/`<cwctype>` completely and use fixed ASCII `"C"`-locale `constexpr` helpers
  (`wse_fold_case`, `wse_upper_case`, `wse_is_ascii_alpha`, `wse_is_ascii_digit`,
  `wse_is_ascii_space`, `wse_is_space`, `wse_trim_ascii`), ensuring identical results across
  MSVCRT, glibc, and Android Bionic regardless of host locale.
- **`sleep_ms` (`3026`):** routes through optional `WseVmState::sleep_ms_fn(sleep_ms_user, ms)`
  (`WseSleepMsFn`) when registered by the embedder/platform layer, falling back to
  `wse::platform::sleep_ms` when `nullptr` so headless/event-loop callers never stall unless
  intended.
- **`get_time` (`3022`):** reads `WseVmState::wall_clock` (`wse::platform::WallClock`) when the
  embedder installs one and `SystemWallClock` otherwise. The local-time form adds
  `utc_offset_seconds()` to the 32-bit UTC value, which is what the retail
  `_localtime64_s` + `_mkgmtime64` pair computes.
- **File opcodes and the `Vfs` (dict `3202`/`3204`/`3209`, JSON, `.wsearray`, savegame MD5):**
  - **Ownership:** `WseObjectManager::vfs` and `WseWorldState::savegames_vfs` are non-owning; the
    embedder keeps each `Vfs` alive while script can run. With a null `Vfs` every file opcode is a
    no-op (loads leave the dictionary as the mode dictates, saves and deletes do nothing).
  - **Case-insensitive names (deviation from a case-sensitive host):** a name is matched ASCII
    case-insensitively, so `Save` and `save` are one file on Linux and Android exactly as on
    Windows; the spelling on disk stays that of the first write.
  - **Missing save directory:** `HostVfs::write_file` creates its root when absent, so the first
    `dict_save`/`array_save_file` into a fresh install succeeds. Directories below the root are
    never created; `mbCheckPath` (`0x1401c5640`) rejects
    separators, so the opcodes cannot ask for one.
  - **Unreported failures:** the save and delete opcodes return no result, so script cannot tell
    whether the write succeeded.
  - **Cost:** a lookup that misses the exact spelling scans its directory once for a
    case-insensitive match; the root's symlink-free form is resolved once per `HostVfs`.
- **Serial file tests:** the `wse_dict`, `wse_array`, and `platform` tests write to temp-directory
  paths and are marked `RUN_SERIAL` in `CMakeLists.txt` so parallel `ctest` runs cannot collide.

### Milestone 3 handlers that are approximations or stubs

These are counted as implemented because they consume their operands and keep scripts running, but
they are **not** verified reconstructions of the cited handlers. Treat their results as
placeholders until each is checked against the v1.1.5.5 disassembly:

- **Constant-value placeholder:** `store_num_free_stacks` (2154) still always stores `10` (the other M3 stubs `1802`/`1930`/`1969` were upgraded in Milestone 4).
- **Simplified helpers in `world_campaign_helpers.h` (no VA evidence yet):** `party_add_template`
  adds the midpoint of each stack's min/max instead of a random count and ignores
  `set_party_creation_random_limits` (1080); `troop_add_merchandise` fills free slots round-robin
  and ignores faction, abundance weighting, and item probability; `troop_equip_items`,
  `troop_sort_inventory`, `troop_ensure_inventory_space`, `party_max_companions`,
  `party_max_prisoners`, and the XP/upgrade helpers use simple local rules.
- **Simplified combat:** `agent_deliver_damage_to_agent` (1722) applies weapon damage times the
  deliverer's damage modifier with no armor, skill, or hit-location model.
- **Semantics still to confirm:** `troop_is_mounted` (152) tests the horse equipment slot (the
  module-system documentation describes a troop-flag test), `entering_town` (36) succeeds when the
  main party is *farther* than `2.3 × radius` from the town, and
  `get_sq_distance_between_positions` (712) scales square metres by `100`.

## Build and test

Windows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1
```

Cross-platform (CMake + CTest + `tools/check_citations.py --self-test`, matching `.github/workflows/ci.yml`):

```sh
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++ -DWSE_BUILD_HEADER_CHECKS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure --no-tests=error
```

Expected host test lines:

```
operand tests: PASS
trigger tests: PASS
savegame tests: PASS
script tests: PASS
wse_bitwise_fp tests: PASS
wse_system tests: PASS
wse_string tests: PASS
wse_dict tests: PASS
wse_array tests: PASS
wse_world tests: PASS
block_vm tests: PASS
PASS  native_world_test
PASS  native_m5_test
PASS  wse_m6_test
platform tests: PASS
module_loaders tests: PASS
headless tests: PASS
lua tests: PASS
asset tests: PASS
audio tests: PASS
input tests: PASS
selftest_host tests: PASS (21/21, ...)
engine host tests: PASS
```

To run the headless Phase 2H validator against a local Warband install:

```powershell
.\engine\build\host\wse_headless.exe "C:\Path\To\Warband" "Modules/Native" --frames 20 --step-ticks 2500000
```

The Android NDK (`aarch64-linux-android29`) header checks, cross-compile build, and `arm64-v8a` SDL3 debug APK (`gradle -p android assembleDebug` -> `libwse2.so` + `libSDL3.so`) also run in `.github/workflows/ci.yml`; see [`STATUS.md`](../STATUS.md) §2 for current gate results.
