# Portable engine sources

Clean-room C++20 reconstruction of the WSE2 engine. Code here is written from the evidence in
`docs/ARCHITECTURE.md` and `docs/SYMBOL_MAP.json`; it contains no lifted machine code.

This file describes **what each module implements and how it deviates from retail**. Coverage
counts and gate results are kept only in [`../STATUS.md`](../STATUS.md); the plan is in
[`../ROADMAP.md`](../ROADMAP.md).

## Layout

```
engine/
  include/wse/core/   math.h, encoding.h, json.h,             pure STL math, UTF/MD5/URL/LE, JSON, and rglString
                      rgl_string.h
  include/wse/io/     text_stream.h, binary_stream.h          module text + savegame streams (incl. checksum writer)
  include/wse/vm/     operand.h, slots.h, statement.h         statement encoding
                      block_vm.h, native_*.h                  block interpreter + native opcodes
                      wse_state.h, execute_wse.h              WSE VM state & top-level dispatcher
                      lua_runtime.h, lua_manager.h            abstract Lua runtime & factory interface
                      ops/{bitwise,system,world,string,fp_expr,fp,dict,array,position,lua_bridge}.h
                      wse_opcodes.h (generated)               WSE extended opcode enum
  include/wse/game/   script.h, trigger.h, savegame.h,        managers, savegame framing, module & scene loaders, campaign helpers, headless runner, audio bridge, and state model
                      world_subsystem_types.h, world_campaign_helpers.h,
                      module_{loader_common,world_loaders,asset_loaders,mission_loaders,bundle}.h,
                      scene_loaders.h, headless_runner.h, audio_runtime.h, world_audio.h, world_input.h
  include/wse/platform/ vfs.h, saf_vfs.h, clock.h,         platform seam (port infrastructure, SAF Vfs, minimal RHI + scene batcher, stereo audio mixer, input & touch overlay, sensors/thermal/FPS performance)
                      sleep.h, log.h, rhi.h, driver_package.h,
                      rhi_brf.h, rhi_scene.h, audio.h, audio_worker.h, input.h, performance.h
  include/wse/asset/  brf.h, dds.h, shader.h, skeleton.h,  .brf / .dds / WAV+OGG+MP3 asset decoders, module resource catalog, shader/material/SPIR-V pipeline, skeletal FK & skinning
                      audio.h, audio_codecs.h
  include/wse/recomp/ x86_decode.h, x86_interp.h,             Phase 5 R0 x86-64 decoder subset, reference interpreter,
                      pe_image.h, r0_inputs.h, x86_synth.h,   PE32+ reader, R0 (d) vectors, synthetic test code,
                      pdb_reader.h, cpu_state.h, runtime.h,   PDB publics, R1 CPU state + hooks, R1 dispatcher/runtime,
                      r2_rig.h                                R2 differential inputs and result hashing
  src/audio/          wse_audio_internal.hpp,               `stb_vorbis` + `dr_mp3` codec backend (`wse_audio`, isolated from public headers)
                      wse_audio_{vendor,codecs}.cpp
  src/lua/            lua_bitop.*, lua_globals_script.hpp,    stock MIT Lua 5.1.5 WSE runtime (`wse_lua`, `<lua.h>` isolated here)
                      wse_lua_{internal.hpp,ops,callbacks,manager}.cpp
  src/recomp/         recomp_llvm.h, r0_spikes.h,             Phase 5 R0 LLVM 18 + LLD lifter & spike runner;
                      lifter_llvm.cpp, r0_spikes.cpp,         R1 Zydis discovery and lifter (core, integer, SSE,
                      discover.*, lift_r1*.{h,cpp},           string families) (`wse_recomp`, built only with
                      llvm_common.h                           `-DWSE_ENABLE_LLVM=ON`)
  tools/              wse_headless.cpp, wse_brf.cpp           headless module CLI validator (`Modules/Native`) + `.brf`/`.dds` inspector
  tests/              one *_test.cpp per suite (+ wse_test_common.h),
                      plus wse_selftest.{h,cpp} and selftest_host_test.cpp, no framework
  fixtures/           hand-written module snippets (never retail files)
  build_host.ps1      Windows host gate
```

The core engine headers under `include/wse/` remain header-only; the Lua runtime (`src/lua/`) compiles into static library `wse_lua` + stock MIT `lua51`, the compressed audio codecs (`src/audio/`) compile into `wse_audio` + `wse_audio_vendor` (`stb_vorbis` + `dr_mp3`), and the Phase 5 R0 translator (`src/recomp/`) compiles into `wse_recomp` when `-DWSE_ENABLE_LLVM=ON` is set via CMake (`../CMakeLists.txt`), CI (`.github/workflows/ci.yml`), or (for the header-only suites) `engine/build_host.ps1`.

## Current modules

| File | Reconstructs | Evidence |
| --- | --- | --- |
| `include/wse/io/text_stream.h` | whitespace token reader and typed decimal parsers | `rglInputTextFileStream` vftable `0x140566030`, `rglTextStream` readers `0x1402afcf0`–`0x1402b00d0`, `mbGame::readTriggers` `0x1400cd080` |
| `include/wse/io/xml.h` | **port infrastructure:** minimal UTF-8 XML DOM parser + entity decoding (`&lt;`, `&gt;`, `&amp;`, `&quot;`, `&apos;`) shared by `scene_loaders.h` and `font.h` | port infrastructure |
| `include/wse/io/binary_stream.h` | little-endian savegame reader/writer + `BinaryChecksumWriter` byte-sum stream | `rglBinaryStream` `0x14055d7b0`; `rglOutputBinaryFileStreamWithChecksum` `0x140566990` (`0x1402c8c70`–`0x1402c8e70`); writers `0x1402aebf0`/`0x1402aec40`/`0x1402aeca0`; readers `0x1402aee50`/`0x1402aee70`/`0x1402aee90` |
| `include/wse/game/savegame.h` | whole `.sav` container framing (`WRDR` magic, `[802..1174]` version, module version, `mbSavegame` header, `new_game.sav` -> `last_savegame_backup.sav` VFS staging + checksum read-back, all `mbGame` sections) | `mbGame::save` `0x1400e1820`, `fillSavegameHeader` `0x1400e1f90`, `mbSavegame::readStream` `0x1400c7510`, `mbLoadSaveWindow::updatePanels` `0x140113d70`, `mbGame::{writeStream,readStream}` `0x1400c9bf0`/`0x1400c8680` |
| `include/wse/core/rgl_string.h` | `RglString` 48-byte SSO string, token cursor (`<= 0x20` skip), underscore/space swaps, int parsers, savegame `"-1"` sentinel | `rglString` (`0x35f5`): `getNextToken` `0x14030f5d0`, `reallocate` `0x14030f720`, `swap*` `0x140310000`/`0x140310030`, `to*64Stream` `0x140310490`/`0x140310570` |
| `include/wse/vm/operand.h` | tagged operand decode and lvalue write-back | `mbStatement::parseOperand` `0x1401c61f0`, `mbStatement::returnValue` `0x1401c6280`, `mbStatementBlock::execute` `0x1401f6b50`, `analyze` `0x1401f68c0` |
| `include/wse/vm/slots.h` | `mbSlots` (`std::vector<int64_t>`) get/set semantics | PDB type `0x1b0f8`; `parseOperand` bounds check; `returnValue` write path |
| `include/wse/vm/statement.h` | statement record and text reader | `mbStatementBlock::readStream` `0x1401f6780`, PDB type `0xb91e` |
| `include/wse/game/world_state.h` | PDB-backed `WseWorldState` entity/slot, skill, troop, item/meta-mesh, faction, quest/note, site/visitor, party/stack, player, agent, prop, team, presentation/overlay, battle event, mission timer, config/var, and edit-mode containers & validators | `mbTroop`, `mbItemKind`, `mbItemKindVariation`, `mbSkill`, `mbPartyTemplate`, `mbFaction`, `mbQuest`, `mbSite`, `mbParty`, `mbnetPlayer`, `mbAgent`, `mbMissionObject`, `mbSceneProp`, `mbTeam`, `rglMesh`, `rglMetaMesh`, `mbGame`, `mbMission` |
| `include/wse/game/world_campaign_helpers.h` | clean-room troop XP/inventory/merchandise/equipment and party member/prisoner/template/attachment helpers for `WseWorldState` | see `world_state.h` layouts |
| `include/wse/vm/block_vm.h` | statement-block VM, loop frames (`try_for_parties` `0xB`, `try_for_agents` `0xC`, `try_for_prop_instances` `0x10`, `try_for_players` `0x11`, `try_for_dict_keys` `0x12`), `NativeDispatch` + per-opcode `unimplementedOpcodeCounts` + `failOnUnimplemented`, `WseVmState`/`WseWorldState` wiring, recursive `currentDepth` / preserved `currentCaller` / `wse_invoke_script_callback` (`array_sort_custom`), script/trigger VM bridge | `0x1401c62d0`, `0x1401f68c0`–`0x1401f8080`, `0x1401f7812`/`0x1401f7f5e`, `0x1400dbf40`/`0x1400dbfd0`, `0x140244e50`, `0x14020fd00`/`0x140210320` |
| `include/wse/vm/native_ops.h` | core + Milestone 1 native dispatcher (`dispatch_native_statement` + `dispatch_native_statement_ext`, `145` switch opcodes): `33` core comparisons/math/assign (`30–33`, `2100–2133`), `44` `entity_slots`, `35` `positions_and_math`, `8` arithmetic/RNG/trig/gold, `19` `strings`, `display_message`, `troop_is_hero`, `troop_set_health`, `agent_is_human`, `store_troop_faction`, `store_faction_of_party`, entity validity filters | `0x14006faf0`–`0x140330d70` |
| `include/wse/vm/native_entity_ops.h` | Milestone 2 entity/campaign native dispatcher (`dispatch_native_entity_statement`, `108` switch opcodes): player/MP (`47`, `394–408`, `430`), quests (`200–204`, `1113`, `1280–1286`), audio/ground/UI/overlays (`599–600`, `791`, `900–947`, `1107`), sites/factions (`1232`, `1261–1270`), troops (`1505–1550`, `2170–2176`), parties/campaign (`1601–1685`, `2156–2281`) | `0x1401c66c8`–`0x1401de4ad` |
| `include/wse/vm/native_mission_ops.h` | Milestone 2 mission/combat native dispatcher (`dispatch_native_mission_statement`, `66` switch opcodes): agent getters/setters/predicates/AI/combat (`1700–1789`), team & division control (`1790–1796`), scene/props/mission control (`1804–2075`), and mission timers (`2370–2372`) | `0x1401d386e`–`0x1401df510` |
| `include/wse/vm/native_campaign_ops.h` | Milestone 3 campaign/world/troop/party native dispatcher (`dispatch_native_campaign_statement`, `156` switch opcodes): campaign state/achievements/multiplayer/positions (`36–793`), mission/spawning/troops/items (`1003–1570`), parties/stacks/morale (`1604–1694`) | `0x1401c65c8`–`0x1401d5616` |
| `include/wse/game/world_subsystem_types.h` | PDB-backed subsystem structs (`WseFaceKeys`, `WseLightingState`, `WseSkyboxState`, `WsePostFxState`, `WseAudioRuntimeState`, `WseNetworkRuntimeState`, `WseDialogState`, `WseTableauRuntimeState`, `WseInputState`, `WseRayObstacle`, `WseMiscRuntimeState`) + `native_f2i_saturate` and `push_bounded` | `mbFaceKeys` `0x1400c5e70`–`0x1400c5f20`, `mbGame`, `mbMission` |
| `include/wse/vm/native_campaign_ops_2.h` | Milestone 3 combat/item/mission/store/face-key native dispatcher (`dispatch_native_campaign_2_statement`, `128` switch opcodes): agent combat/items/teams/props (`1695–2099`), stores/relations/timers/strings (`2154–2379`), item getters/predicates & face keys (`2700–2751`) | `0x1401d3689`–`0x1401e1697` |
| `include/wse/vm/native_world_ops.h` | Milestone 4 world/face-key/input/troop/party native dispatcher (`dispatch_native_world_statement`, `69` switch opcodes incl. `53` `is_cheat_mode_enabled`, `2180` `store_main_party_wounded`, `2749` `str_store_agent_face_keys`) | `0x1401c671c`–`0x1401e22d0` |
| `include/wse/vm/native_world_ops_2.h` | Milestone 4 mission-combat/audio-tableau/store native dispatcher (`dispatch_native_world_2_statement`, `69` switch opcodes across `mission_combat_and_teams`, `audio_particles_and_tableau`, and `game_state_and_stores`) | `0x1401c67a6`–`0x1401df785` |
| `include/wse/vm/native_world_ops_3.h` | Milestone 4 `other_native` dispatcher (`dispatch_native_world_3_statement`, `80` switch opcodes covering input/options, spawn effectors/bans, MP/UI/difficulty, dialog/notes/quests/encounters, mission/tableau/talk, lighting/skybox/postfx) | `0x1401c6b62`–`0x1401dfd3b` |
| `include/wse/game/trigger.h` | trigger/simple-trigger records, loaders, savegames, and event-key runtime state machines | `0x140244990`, `0x1402446c0`, `0x14020ffe0`, `0x14020fc30`, `0x1400cd080`, `0x1400cd430`, `0x140244af0`, `0x140244ce0`, `0x140210120`, `0x140244e50`, `0x14020fd00`, `0x140210320` |
| `include/wse/game/script.h` | `mbScript`/`mbScriptManager` readers, 65-entry name table, and script mapping | `0x14020f820`, `0x14020f940`, `0x14020fb20`; globals `0x1406a0af0`/`0x1406a0af8`/`0x1406a0afc`; table `0x140616ad0`; formats `0x14055d6b0`/`0x14055d6c0` |
| `include/wse/game/module_loader_common.h` | `ModuleLoadError`, token/version header helpers, BOM/comment-stripping INI parsers, and VFS text loader helpers (strictly via `wse::platform::Vfs` + `wse::io::TokenReader`); `[section]` headers are recognised after comment stripping, and `read_wrapping_u32_token` wraps negative literals in unsigned arithmetic | `rglIni` (`0x1402a6ff0`), `rglSectionlessIni`, `mbCoreGame` |
| `include/wse/game/module_world_loaders.h` | world/campaign loaders (`skills`, `factions`, `troops` v1/v2, `item_kinds` v1/v3, `party_templates`, `parties`, `scenes`, `quests`, `info_pages`, `strings`, `quick_strings`, `game_variables`, `module.ini`, `module_config.ini`, `Data/item_modifiers.txt`) | `0x1402a6ff0`, `0x140093f80`, v1.1.5.6 stream readers cited in header |
| `include/wse/game/module_asset_loaders.h` | asset/visual loaders (`meshes`, `actions`, `skins` (`0x140213810`), `sounds` v2/v3, `music`, `particle_systems`, `postfx`, `Data/{flora_kinds,ground_specs,skyboxes}.txt`) | v1.1.5.6 stream readers cited in header |
| `include/wse/game/module_mission_loaders.h` | interactive/mission loaders (`map_icons`, `scene_props`, `menus`, `presentations`, `mission_templates`, `tableau_materials`, `conversation` v1/v2) | v1.1.5.6 stream readers cited in header |
| `include/wse/game/module_bundle.h` | `WseOwnedStringBanks`, `ModuleBundle`, `ModuleLoadOptions`, and `load_module()` orchestrator populating `WseWorldState`, `ScriptManager`, triggers, and `VmContext` string banks; `load_module()` first resets module-derived `WseWorldState` (safety deviation: a reload never inherits the previous module's optional files), and the bundle is move-only because `VmContext` holds views into it | `mbGame::readModuleFiles` cited in header |
| `include/wse/game/headless_runner.h` | **port infrastructure:** `HeadlessRunOptions`, `HeadlessRunReport`, `HeadlessSession`, and `run_headless_module()` / `run_headless_session()` executing `load_module`, `game_start`, and simulated trigger frames via `Vfs` and `LogSink`; unimplemented opcodes stop and fail the run only when `fail_on_unimplemented` is set (otherwise they are counted and reported) | clean-room orchestration over `module_bundle.h`, `trigger.h`, `block_vm.h` |
| `include/wse/asset/{brf,dds,shader,skeleton,font}.h` | `.brf` container decoder (all 8 resource types, `MetaMesh` LOD grouper, `module.ini` resource catalog; `skeleton_anim` `(qx,qy,qz,qw)` wire order), `.dds` parser/RGBA8 decoder (`BC1`–`BC5`, uncompressed/`L8`/`L16`/`A8L8`/`A8`, `DX10`) + Warband `Vfs` texture resolver, P5 effect search / `wse2_shaders.{brf,ini}` / `rglShader` suitability / `rglMaterial` state / `GLShaders` + WSE2 SPIR-V 1.0 cache pipeline, P9 skeletal bind-pose FK evaluation + CPU linear-blend skinning, and P9 `Data/font_data.xml` bitmap font loader + `<c=...>`/word-wrap text layout (`rglFont` `0x140297a20`, `0x140298100`, `0x14029b650`, `0x14029b7d0`) | `rglResourceManager`, `rglMesh`, `rglMetaMesh`, `rglTexture`, `rglShader`, `rglMaterial`, `rglPhysicsMaterial`, `rglBody`, `rglSkeletonModel`, `rglSkeletonAnimation`, `rglSkeleton`, `rglBone`, `rglQuaternion`, `rglFont`, `rglGraphicsContext::initEffect`, `rglPostProcess::initEffect` |
| `include/wse/game/scene_loaders.h` | Phase 2A world map manifold (`map.txt` + barycentric surface height/material query), `.sco` scene objects (`mbSite::readSceneObjFile`, `"spr_"` normalization, AI mesh + `0xff4ad1a6` ground paint, non-prunable world instantiation), and `Data/skeleton_bodies.xml` bone/capsule parser | `mbGame::readMap` (`0x1400d9b60`), `rglManifold`, `mbSite::readSceneObjFile` (`0x140210d60`), `mbMissionObject::readStream` (`0x140152d60`), `rglAiMesh` (`0x140252780`), `mbSkeletonManager` (`0x140211d20`) |
| `include/wse/game/ui_runtime.h` | Slice P9 Part 2 localization (`LanguageTable`), layout variables (`GameVariablesTable`), `UiWidget` tree & property applicator (`rglReadWidgetProperties` `0x140329440`), and `UiSession` initial/menu/notes/presentation controllers (`-60`..`-63` triggers, modal `0x15`/`0x16` closing) | `mbLanguageManager` (`0x14028d850`, `0x14028e460`), `rglWidget` (`0x14031f360`, `0x14031e0d0`, `0x14031f9e0`, `0x14031fed0`, `0x140329440`), `mbMenuWindow` (`0x1401203a0`, `0x140121270`), `mbPresentationContainer` (`0x140209930`, `0x140209040`), `mbPresentationWindow` (`0x140175050`), `mbNotesWindow` (`0x1401616a0`), `mbGameScreen` (`0x1400f0de0`) |
| `include/wse/platform/rhi_scene.h` | **port infrastructure (Slice P9 Part 1):** Z-up transform/camera projection helpers, 16-bit chunked `WorldMapManifold` terrain triangulation (`build_terrain_chunks`), and multi-draw `BrfSceneRenderer` for terrain, `.sco` props, and dynamically skinned `BrfMesh` instances over `RhiDevice` (caching rest vertices, skin weights, and static IB per slot) | port infrastructure over `scene_loaders.h`, `skeleton.h`, `rhi.h`, and `rhi_brf.h` |
| `include/wse/platform/rhi_ui.h` | **port infrastructure (Slice P9 Part 2):** `LoadedUiAssets` + `load_native_ui_assets` and `UiRhiRenderer` rendering 2D UI widget trees, stretched BRF UI meshes, and batched glyph quads over `RhiDevice` (`[0,1]x[0,0.75]` Y-up ortho, bottom-up V -> top-down DDS `1 - v` UV conversion) | port infrastructure over `font.h`, `ui_runtime.h`, `brf.h`, `dds.h`, and `rhi.h` |
| `include/wse/asset/{audio,audio_codecs}.h`, `src/audio/` | RIFF/WAVE PCM (`8`/`16`/`24`/`32`-bit & IEEE float/`0xFFFE`) decoder, retail 4-step sound & 2-step music path resolvers, `mbSoundSample` / `mbMusicTrack` flag/priority/volume/culture helpers, and static `wse_audio` (`stb_vorbis` v1.24 + `dr_mp3` v0.7.5) memory + streaming decoder backend isolated from public headers | `mbSoundSample::readStream` `0x140332580`, `mbSoundSample::playSound` `0x140332a70`, `mbSoundManager::readStream` `0x1403330a0`, `mbMusicTrack::readStream` `0x140290870`, `mbMusicManager::readStream` `0x140290c30` |
| `include/wse/game/{audio_runtime,world_audio,world_input}.h` | non-owning `WseAudioRuntime` VM hook + `WorldAudioBridge` (`599..604`, `609`, `615..616`, `1750`, `1808`, `1881..1882`, `3618`), plus `WorldInputBridge` syncing `WseInputManager` into `WseWorldState` (`65`, `70..73`, `75`, `77..78`, `3100..3102`, `map_zoom_delta` -> `map_camera_distance` via `exp(-delta)`) | clean-room bridge over `0x140057170`, `0x1400e3a60`, `0x1401544c0`, `0x140290da0`, `0x140333880`, `0x1401c68d2`, `0x1401e6bc2` |
| `tools/wse_headless.cpp` | portable Phase 2H CLI runner (`<warband_root> [module_rel_dir] [--frames N] [--step-ticks T] [--allow-unimplemented] [--no-ui] [--audio] [--preload-audio]`; prints `presentations_opened` / `notes_opened` / `final_window`) using `HostVfs` + `StderrLog` | Phase 2H headless `Native` exit runner |
| `tools/wse_brf.cpp` | portable Slice P3/P5 / Phase 2G CLI inspector (`--brf <file>`, `--dds <file>`, `--dir <dir>`, `--root <warband_root> [--module <name>]`, `--json`) using `HostVfs` | Slice P3/P5 `.brf` / `.dds` / shader & material corpus validator |
| `include/wse/vm/wse_opcodes.h` | generated enum of the 440 WSE-extended operation names in the retail dispatch range | `mbStatement::executeWSE` `0x1401e4f40`; names from `header_operations_addon.py` |
| `include/wse/core/{math,encoding,json}.h` | pure STL math vectors/transforms/MT19937, UTF-8/UTF-16/MD5/URL/path/LE codecs, minimal JSON parser/serializer | `rglTransform`, `rglMersenneTwister`, `UrlEscapeA`/`UrlUnescapeA`, `Json::Reader`/`Writer` |
| `include/wse/vm/wse_state.h` | opcode mask, operand/effect structs, `WseStringBanks`, `WseOptionsState`, `WseObjectManager`, `parseString`, shared file helpers | `executeWSE` `0x1401e4f40`, `0x140072210`, `0x1401c62d0` |
| `include/wse/vm/ops/{bitwise,system,world,string,fp_expr,fp,dict,array,position}.h` | per-family `executeWSE` dispatch handlers (2800–2809, 3000–3045/3619/3622/4300–4305/4606, 3340–3341, 3600–3911/4600–4609, 4200–4246, `feval` 4400–4438, dict 3200–3222, array 5000–5022, pos vec 4100–4104) | `executeWSE` `0x1401e4f40`; `0x1400a6d70`–`0x1400ad700`; `0x1401e5745`–`0x1401ec6ab`; `0x1401ef26f`–`0x1401efe0d`; `0x1401f1b0c`–`0x1401f1dac`; `0x1401f5a20`–`0x1401fdf00`; `0x140246190`–`0x140246357`; `0x14024f640`; `0x1402a2760`–`0x1402b6720`; `0x14034f100`–`0x140351660` |
| `include/wse/vm/lua_{runtime,manager}.h`, `src/lua/` | isolated stock MIT Lua 5.1.5 `WseLuaRuntime` implementing sandboxed `io`/`os`/`package`/`load*` over `Vfs`/`Clock`/`LogSink`, `bit` module, clean-room `LuaGlobals`, `msfiles/` + `variables.txt` parsers, 36 `game.*` + 7 `_G.*` callbacks, op/script/event hooks, and `lua_bridge.h` (`5100`–`5114`) integration | `WSELuaManager` (`0x14033db90`, `0x14033de50`; see `lua_runtime.h`, `lua_manager.h`) |
| `include/wse/vm/execute_wse.h` | top-level `execute_wse()` umbrella dispatching across `wse/vm/ops/*.h` | `executeWSE` `0x1401e4f40` |
| `include/wse/platform/{vfs,saf_vfs,clock,sleep,log,rhi,driver_package,rhi_brf,audio,audio_worker,input,performance}.h` + `../platform/vulkan/driver_loader.{h,cpp}` + `../platform/sdl/{sdl_audio,sdl_input}.{h,cpp}` | **port infrastructure + P6/P7/P8 reconstructions:** `Vfs` (case-insensitive, `/` and `\\` separators, root-confined, optional atomic `rename_file`; `HostVfs` ASCII-folded dir-cache keys with a same-directory check) with `MemoryVfs`, `HostVfs`, and `SafVfs` (`SafTreeBackend`, `MemorySafBackend`, one-time tree index, `import_to`, `scan_warband_modules`, `verify_module_scripts`); `Clock` in the engine's 100,000-per-second ticks with `SteadyClock`/`ManualClock`; `WallClock` (Unix seconds + UTC offset) with `SystemWallClock`/`ManualWallClock`; blocking `sleep_ms`; `LogSink` with stderr, capture, and null sinks; `RhiDevice` + `SoftwareRhiDevice` (`rhi.h`, standard library only), AdrenoTools `meta.json` parser, ELF `.dynstr` `libpanvk_hw.so` patcher, and atomic package staging (`driver_package.h`), BRF/DDS textured mesh helpers + cached `BrfMeshRenderer` (`rhi_brf.h`), `VulkanDriverLoader` (`load_system`, direct-ICD v5 / custom driver preload, `MockVulkanIcd` v5, `VulkanRhiDevice`), `AudioDevice`/`NullAudioDevice`/`CaptureAudioDevice` + `WseAudioMixer` (`audio.h`, stereo 2D/3D mixer + music scheduler) + `SDL_OpenAudioDeviceStream` backend (`sdl_audio.{h,cpp}`), `InputBackend`/`NullInputBackend`/`SyntheticInputBackend`, `WseGameKeyManager`, `WseInputManager` (`InputContext`, virtual stick/look/4-dir attack & block swipes/command radial, opt-in low-pass filtered gyro aim (smoothing defined per 60 Hz frame and scaled by `dt`, last rate held `50 ms` between sensor samples, `gyro_look_rates` remapping natural-orientation axes per `ScreenRotation`), two-finger `CampaignMap` pinch-to-zoom (`map_zoom_delta` = `ln` of the spread ratio), `TouchLayoutEditor`, per-module `Vfs` JSON profiles), `FreeCameraController` (`input.h`), SDL3 input/sensor backend (`sdl_input.{h,cpp}`: phone gyro remapped for the display rotation, controller gyro via `SDL_SetGamepadSensorEnabled` preferred when present), `make_parent_dirs` (`vfs.h`, used by the settings and input-profile savers), and `performance.h` (`FrameRateCap` 30/60/90/120 Hz + uncapped, `PresentModePreference`, hysteretic `ThermalPerformanceController` -> resolution scale / LOD bias / thermal FPS step-down (immediate escalation, one-tier-per-update recovery, last valid headroom held across headroom-less readings), drift-free `FramePacer`, `FrameTimeStats` (rolling average / 1% low), `RuntimePerformanceHub`, persisted `Vfs` JSON settings) | none for core seam interfaces (replaces Win32 imports in `docs/ARCHITECTURE.md` §6); `WseAudioMixer` reconstructs `0x140290da0`, `0x140290fc0`, `0x1402910c0`, `0x140332a70`, `0x140333450`–`0x140334250`; `WseGameKeyManager` reconstructs `0x1402ad5d0`, `0x1400e96f0`–`0x1400ef670`, `0x1406a0c00` (design in `docs/PLATFORM_PLAN.md`) |
| `tests/platform_test.cpp` | host tests for path normalisation and escape rejection, one `Vfs` contract run across `MemoryVfs`, `HostVfs`, and `SafVfs`, `SafVfs` one-time index / `scan_warband_modules` / `verify_module_scripts` / `import_to`, `Vfs` → `TokenReader`, clocks, log sinks, `test_rhi_and_driver_loader` (Turnip/PanVK `meta.json`, `.dynstr` `libpanvk_hw.so` patcher, Mock ICD v5, BRF/DDS textured mesh screenshot & Z-buffer tests), `test_p5_shader_modules_d3d9_and_material_rhi` (`PresentationPath` WSI vs `AHardwareBuffer` bridge, `VkShaderModule` lifecycle, D3D9 projection/half-pixel/sRGB helpers, normal/specular/alpha-test/blend material RHI), `test_p4_review_fixes` (mode ids, name allow-list, nested `meta.json`, atomic re-staging, front face, handle wrap, device generations, mock object accounting), `test_p5_rhi_review_fixes`, and `test_p8_performance_and_frame_pacing` (`FrameRateCap`, `PresentModePreference`, `PerformanceSettings` JSON round-trip & sanitization, `ThermalPerformanceController` headroom/status hysteresis & toggles, `FramePacer` 60/90/120 Hz drift-free cadence over `ManualClock`). The real host Vulkan loader is exercised only with `WSE_TEST_SYSTEM_VULKAN=1` | temp directory + in-memory trees |
| `tests/operand_test.cpp` | host tests for tags, decode, store, slots, tokenizer, statements | local test doubles only |
| `tests/trigger_test.cpp` | host tests for trigger loaders and event-key state machines | local test doubles only |
| `tests/savegame_test.cpp` | host tests for binary streams, checksum writer, `RglString` (incl. moved-from reuse), trigger savegames, whole `.sav` container round-trips, validation, VFS backup rotation, transactional / hostile-input loads, a deterministic mutation fuzz, and torn-write read-back | `MemoryVfs`, `TruncatingVfs` + local test doubles |
| `tests/script_test.cpp` + `fixtures/scripts.txt` | host tests for script records, header errors, duplicate mapping, and missing-name reporting | local fixture mirroring the retail grammar |
| `tests/wse_test_common.h` + `tests/wse_{bitwise_fp,system,string,dict,array,world}_test.cpp` | host tests for opcode enum values, all implemented handlers (including `*_slot_gt`, scalar/config/skill/savegame MD5, troop HP cap/nibble/proficiency, item/cur_item, edit-mode selection/highlight, `agent_{set,get}_action_speed_modifier` `3340–3341`, party `3900–3911`, `feval`/`parseString`, `.wsedict`/INI/JSON/URL dict I/O, `.wsearray` binary I/O, path sandboxing, natural merge sort, and array relations), operand bounds, and opcode masking | local test doubles only |
| `tests/block_vm_test.cpp` + `tests/native_world_test.cpp` | host tests for analyze/modifiers/loops (live entity iterators `11/12/16/17`, `try_for_dict_keys`), `currentCaller` preservation, default local-variable zeroing (`0x140618819`), core math/scripts/recursion/return values/positions/trigger VM integration/`feval`, `array_sort_custom` script callbacks, `WseWorldState` script integration, unimplemented histogram/`failOnUnimplemented`, Milestone 1 `entity_slots`/`arithmetic`/`positions_and_math`/`strings` suites, Milestone 2 `108` entity/player/quest/UI/site/troop/party + `66` agent/team/prop/mission/timer opcodes, Milestone 3 `284` campaign/mission/item/troop/party/face-key opcodes, and Milestone 4 `218` opcodes + `473/477/1004/1070/1104/1302/1491` non-switch/fallback + `1802/1930/1969` upgrades | local test doubles and module fixtures |
| `tests/module_loaders_test.cpp` | host tests for INI/BOM/comment parsers, individual world/campaign/asset/mission/Data loaders, error diagnostics, full `load_module()` + VM execution round-trips over `MemoryVfs` and `HostVfs`, and reload (module A then B) world-state reset + move-only string storage | in-memory + temp-directory fixtures |
| `tests/headless_test.cpp` | host tests for `run_headless_module()` / `run_headless_session()` (`game_start` + multi-frame triggers over `MemoryVfs` and `HostVfs`, loud unimplemented-opcode failure, compatibility-mode continuation past unimplemented opcodes, and missing `game_start` / load error diagnostics) | in-memory + temp-directory fixtures |
| `tests/lua_test.cpp` | host tests for Lua 5.1.5 stdlib/`bit`/`vector3`/`pos`, `msfiles/` + `variables.txt` parsers, `game.*` / `_G.*` callbacks, trigger/item/prop/prsnt/psys/iterator/hook paths, opcodes `5100`–`5114` + 9 `On*` events, and sandboxed `HostVfs` headless `main.lua` execution | `MemoryVfs` + temp-directory `HostVfs` fixtures |
| `tests/asset_test.cpp` | host tests for all 8 `.brf` resource types (`rfver` 0 and `rfver` 1), `MetaMesh` LOD grouping, `module.ini` resource catalogs over `MemoryVfs`, `.dds` block/uncompressed/`L16`/`DX10` decoding, Warband texture resolution priority, and P5 effect/INI/shader/material resolution + `GLShaders`/WSE2 SPIR-V persistent caching | synthetic binary + `MemoryVfs` fixtures |
| `tests/audio_test.cpp` | P6 review regressions (re-entrant end callbacks & sibling-slot reuse, `set_config` during playback, voice-pitch curve & overflow, whole-sample random start, same-track `play_track`, stream/predecoded parity across chunk boundaries, loop-point interpolation, surround fold, oversized WAV `data`, config sanitising, clock regression, `615` after failure, prop/agent channel replacement, sounds following agents) plus host tests for RIFF/WAVE PCM (`8`/`16`/`24`/`32`-bit & float/`0xFFFE`), OGG Vorbis + MP3 decoding & chunked streaming (`wse_audio`), 4-step sound & music lookup order, `WseAudioMixer` 3D constant-power panning / attenuation / LOD / priority eviction / occlusion hysteresis / deterministic hash / `0.5s`+`5.0s` music scheduler, VM/world audio integration (`599..604`, `609`, `615..616`, `1750`, `1808`, `1881..1882`, `3618`), and opt-in retail `Native` + `Phantasy Calradia` audio gate | synthetic WAV/OGG/MP3 + `MemoryVfs` + `CaptureAudioDevice` (+ optional `HostVfs` retail install) |
| `tests/input_test.cpp` | Slice P7/P8 tests for `get_raw_key_name` (`0x1402ad5d0` 125 keys), `WseGameKeyManager` (`0x1400e9800` 75 built-ins + Custom1..20 + modifier chords + gamepad alts + `getKeyData` + `storeKeys`/`restoreKeys`), `InputContext` & gamepad attach overlay hiding, multi-touch stick/look/4-direction attack & block swipes/command radial, `TouchLayoutEditor`, per-module `Vfs` JSON profiles (`%.9g` round-trip & corrupt rejection), `FreeCameraController` driving the P4 `BrfMeshRenderer` viewer, `WorldInputBridge` (`65`, `70..73`, `75`, `77..78`, `3100..3102`), plus P8 opt-in low-pass filtered `Mission` gyro aiming, `CampaignMap` two-finger pinch-to-zoom, and backwards-compatible v1 JSON profile serialization | `SyntheticInputBackend` + `MemoryVfs` + `SoftwareRhiDevice` |
| `tests/scene_test.cpp` | host & opt-in retail tests for `WorldMapManifold` (`map.txt` material unpack & barycentric surface height query), `.sco` v0..v4 (`spr_` normalization, AI mesh, `0xff4ad1a6` ground paint, `416` survival), `Data/skeleton_bodies.xml` parser (comments, twist symmetry, deg->rad), `skeleton.h` FK & CPU linear-blend skinning (incl. analytical root motion), multi-draw `BrfSceneRenderer` terrain + prop + skinned render over `SoftwareRhiDevice` (incl. static IB reuse), and retail `Native` `skeletons.brf` + `ani_stand.brf` + `body_meshes.brf` render | synthetic binary/text + `MemoryVfs` + `SoftwareRhiDevice` (+ optional `HostVfs` retail install) |
| `tests/ui_test.cpp` | Slice P9 Part 2 tests for `wse/io/xml.h` round-trip/entity/error handling, `wse/asset/font.h` `Data/font_data.xml` glyph metrics & `<c=...>`/word-wrap/alignment text layout, `wse/game/ui_runtime.h` `LanguageTable`, `GameVariablesTable`, `UiSession` initial/menu/notes/presentation windows (`-60`..`-63` triggers + modal `0x15`/`0x16` close regressions), `wse/platform/rhi_ui.h` synthetic upright glyph/BRF UV orientation & highlight rendering over `SoftwareRhiDevice`, and retail `Native` main menu + game menu rendering | synthetic XML/CSV/RGBA8 + `MemoryVfs` + `SoftwareRhiDevice` (+ optional `HostVfs` retail install) |
| `include/wse/recomp/x86_decode.h` | Phase 5 R0 x86-64 decoder subset (`Reg`, `Op`, `Cond`, `Operand`, `Insn`, `decode_insn`: moves, `movsx`/`movzx`, `lea`, `push`/`pop`, ALU group except `adc`/`sbb`, `test`, `inc`/`dec`, conditional/unconditional near branches, `ret`, `nop`) | `rglString::indexOf` `0x1403103f0` leaf subset (port infrastructure) |
| `include/wse/recomp/x86_interp.h` | Phase 5 R0 reference interpreter and function CFG decoder (`CpuState`, `DecodedFunction`, `decode_function`, `eval_cond`, `Interpreter` executing decoded `Insn` records over host memory as the lifter oracle) | port infrastructure |
| `include/wse/recomp/pe_image.h` | minimal bounds-checked PE32+ reader (`PeImage`, `PeSection`, `PeFunctionRange`, `parse_pe`, `map_pe_sections`: headers, sections, `.pdata` function ranges, preferred-base mapping) | port infrastructure |
| `include/wse/recomp/r0_inputs.h` | R0 (d) deterministic 512-case test vectors (`make_r0_index_of_cases`), `RglStringFake`, `r0_index_of_model`, `fnv1a_results`, and Win64 `GuestStack` | `rglString::indexOf` `0x1403103f0` (and `rglString` PDB type `0x35f5`, `docs/ARCHITECTURE.md`) |
| `include/wse/recomp/x86_synth.h` | synthetic x86-64 test function generator (`make_synthetic_function`) in the R0 decoder subset for R0 (c) timing and host tests | port infrastructure |
| `src/recomp/{recomp_llvm,r0_spikes}.h`, `src/recomp/{lifter_llvm,r0_spikes}.cpp` | Phase 5 R0 LLVM 18 + LLD lifter, optimizer, object emitter, embedded linker (`Translator`, `emit_probe_object`, `link_shared_object`), and spike runner (`run_r0_spikes`; `wse_recomp` under `-DWSE_ENABLE_LLVM=ON`, `libwse_r0.so` via `../platform/android/r0_jni.cpp` for `../android/app/src/main/kotlin/org/wse2/android/launcher/R0Spikes.kt`) | port infrastructure (`rglString::indexOf` `0x1403103f0` checked for R0 (d); design in `docs/RECOMP_PLAN.md`) |
| `tests/recomp_test.cpp` | host suite `recomp`: `decode_insn` operands/rejections, `Interpreter` flags, `decode_function`, synthetic R0 (c) model match, `parse_pe`/`map_pe_sections`, and R0 (d) vectors/FNV-1a | synthetic x86-64 bytes + synthetic PE32+ |
| `include/wse/recomp/cpu_state.h`, `include/wse/recomp/runtime.h` | R1 `CpuState` (448 B: GPRs, `rip`, flags, XMM, MXCSR, hook table) and `RuntimeHooks`; `Runtime`: the `wse_fn_table` dispatcher, `import_call` shims, `bad_return` / `unsupported` / segment faults, `div128` | port infrastructure |
| `src/recomp/discover.{h,cpp}`, `src/recomp/lift_r1*.{h,cpp}`, `src/recomp/llvm_common.h` | R1: Zydis 4.1.0 function discovery (control flow from each entry, MSVC and clang jump tables, tail calls) and the LLVM 18 lifter (`R1Translator`; families `lift_r1_int.cpp`, `lift_r1_sse.cpp`, `lift_r1_string.cpp`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R1 design") |
| `include/wse/recomp/shim_advapi.h`, `include/wse/recomp/shim_user32.h`, `include/wse/recomp/shim_net.h`, `include/wse/recomp/shim_system.h`, `include/wse/recomp/shim_d3dx.h` | R4 shims: the registry and `Crypt*` hashes (`shim::Advapi`); fake windows, message queues, cursor, clipboard, IMM32/GDI32 (`shim::User32`); the offline network: Winsock, WinHTTP, WinINet, urlmon (`shim::Net`); dbghelp, ole32, OLEAUT32, SHLWAPI, SHELL32, PSAPI, XInput, the last KERNEL32 imports (`shim::SystemDlls`); D3DX math (`shim::D3dxMath`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R4 design") |
| `include/wse/recomp/shim_d3d9.h`, `include/wse/recomp/com_vtables.h` | R4 recording D3D9 device, the D3DX texture/effect functions and DirectInput 8 as COM objects whose vtables are callable addresses (`shim::D3d9`); vtable orders and IIDs generated by `../tools/gen_com_vtables.py` | port infrastructure (design in `docs/RECOMP_PLAN.md` "R4 design") |
| `include/wse/recomp/shim_modules.h` | R4 multi-module runtime: the module registry (translated images and shim-backed system DLLs, `shim::Modules`), cross-module import linking, `DllMain` at process attach, and the module and resource shims (`GetModuleHandle*`, `GetProcAddress`, `LoadLibrary*`, `GetModuleFileName*`, `RtlPcToFileHeader`, `FindResource*`, `LoadResource`, `SizeofResource`); `pe_image.h` adds `read_pe_exports`, `apply_pe_relocations`, `find_pe_resource` | port infrastructure (design in `docs/RECOMP_PLAN.md` "R4 design") |
| `include/wse/recomp/shim_files.h` | R4 shims: files, directories, searches, paths, file mappings, standard handles, `SHGetFolderPath` over `Vfs` mounts (`shim::Files`, `shim::FilesConfig`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R4 design") |
| `include/wse/recomp/shim_strings.h`, `include/wse/recomp/shim_memory.h` | R4 shims: code pages, conversions, case and character types, locale, date/time formats, `FormatMessage` (`shim::Strings`); heaps, Local/Global and virtual memory (`shim::Memory`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R4 design") |
| `include/wse/recomp/shim_time.h`, `include/wse/recomp/shim_crt.h` | R4 shims: time and counters (`shim::Time`), CRT support, process and system information (`shim::Crt`, `shim::ProcessConfig`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R4 design") |
| `include/wse/recomp/threadpool.h` | R3 thread-pool timers and waits, `RegisterWaitForSingleObject` (`threadpool::Pool`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R3 design") |
| `include/wse/recomp/threads.h`, `include/wse/recomp/kernel.h` | R3 threads and sync: `CreateThread` and the thread calls on host threads (`threads::Threads`); the handle table, waits, events, mutexes, semaphores, critical sections, condition variables, SLists, `Sleep` (`kernel::Kernel`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R3 design") |
| `include/wse/recomp/teb.h` | R3 thread environment: TEB/PEB in guest memory, static TLS from the TLS directory (`read_pe_tls` in `pe_image.h`), TLS callbacks, and the `Tls*`, `Fls*`, `GetLastError`/`SetLastError`, `GetCurrentThreadId`/`ProcessId`/`Thread`/`Process` shims (`teb::Process`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R3 design") |
| `include/wse/recomp/unwind.h`, `include/wse/recomp/seh.h` | R3 exceptions: the Win64 virtual unwinder over each translated image's `.pdata`/`.xdata` (R4: `Exceptions::add_image`) (`lookup_function`, `virtual_unwind`: partial prologs, epilogs from their instructions, chained info, machine frames) and the Windows layer under the exe's own C++ EH (`seh::Exceptions`: `RaiseException`, `RtlUnwindEx` with unwind consolidation, `RtlUnwind`, `RtlRestoreContext` (R4), `RtlVirtualUnwind`, `RtlLookupFunctionEntry`, `RtlCaptureContext`) | port infrastructure (design in `docs/RECOMP_PLAN.md` "R3 design") |
| `include/wse/recomp/r2_rig.h`, `src/recomp/r2_reference_v1156.inc` | R2 differential rig: per-case inputs at fixed buffer/stack addresses, buffer fill/restore, hashing of `rax`, `xmm0` and changed buffer words; the PC-recorded table (generated by `../tools/r2_pc_record.cpp`) the phone checks against | port infrastructure (design in `docs/RECOMP_PLAN.md` "R2 design") |
| `tests/recomp_llvm_test.cpp` | host translator test (`recomp_llvm`, built with `-DWSE_ENABLE_LLVM=ON`, CI job `recomp-host`): lifts/optimizes/links synthetic functions, runs the R1 differential corpus (`tests/recomp_corpus/*_corpus.c` compiled for Win64 into `corpus.exe` and, from R4, `corpus_dll.dll`, mapped at another base and linked through `shim::Modules`: every export native vs translated) and R0 spikes (a)–(c) against `Interpreter` | synthetic x86-64 bytes and our own C only |
| `tests/wse_selftest.{h,cpp}`, `tests/selftest_host_test.cpp` | shared aggregate runner invoking all `24` core suites (`WSE_SELF_TEST_LIBRARY=1`) for both host verification (`selftest_host_test`) and the Android SDL3 `libwse2.so` app shell (`platform/sdl/sdl_selftest_main.cpp`); `run_all_suites` takes an optional stop callback polled between suites. In the CMake build each suite is compiled once into `wse_selftest` and every `<name>_test` executable is `tests/suite_main.cpp` linked against it | all 24 suites + `fixtures/scripts.txt` |

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

### Entity container lifetimes (Phase 2E)

`WseWorldState` (`wse/game/world_state.h`) owns the four live containers:

- **Parties** (`rglHashVector<mbParty>`, `getFreeItem` `0x1400e8890`): `parties` is indexed by
  party key; `allocate_party` hands out `max(next_party_key, parties.size())` (`m_nextKey`
  `+0x138f0`), filling any gap with tombstones, so handles are never reused. Saves restore
  `m_nextKey` and place each slot's party at its stored key, which may exceed `m_numSlots` once
  retail recycled hash-vector slots (a key at or above `m_nextKey` is `corrupt_record`); `remove_party`
  (`mbGame::removeParty` `0x1400ddaf0`) leaves a tombstone (`active = false`, `pf_disabled`) and
  clears parent/child links, AI behavior targets, agent/player links, and the encountered /
  camera-follow party ids.
- **Agents / mission objects** (`rglStableVector`, `createItem` `0x14014bae0` / `0x14014c1b0`,
  `remove` `0x14014b990` / `0x14014bf40`): freed interior slots are reused LIFO and trailing free
  slots are trimmed. Because ids are reused, `remove_agent` / `remove_mission_object` first clear
  every world reference to the id (`unlink_agent_references`: team leader, order listener,
  division leaders, look / AI targets, offers, cached enemies, horse/rider in both directions,
  `parent_agent_no`, mission camera, dialog speaker, players; for objects: spawn effectors, agent
  attachments, standing object, attached missiles, edit-mode selection).
- **Players** (`mbnetPlayer::initialize` / `clear` / `create`): `disconnect_player` resets the
  slot to `UNASSIGNED` / team 2 but keeps `mp.saved_ban`, so `ban_player_using_saved_ban_info`
  (468) still bans a player who was kicked while the ban poll ran.

`step_world_frame` runs after the trigger managers: on the campaign map it consumes one queued
window (`consume_campaign_queued_window`: `0x12` menu, `0x0f` presentation, and `0x10` notes open
windows `0x12` / `0x16` / `0x15` and sync `m6.ui.active_window_id`); in a mission it retires any
campaign window id (map / menu / notes / presentation -> `-1`), removes agents 30 s after
`agent_fade_out` (1749), and removes prunable objects whose timer expired. `start_presentation`
(900) queues its `0x0f` window only on the campaign map. `multiplayer_clear_scene` (416) removes all
agents and every prunable object (`m5.enable_pruning`, `+0x1d5`) and keeps scene-placed and
unpruned props; `scene_prop_set_prune_time` (1819) arms `enable_pruning` for a positive time
(`setPruneTime` `0x140153f08`).

Safety deviations: `remove_party` refuses the main party; `attach_party` (and
`party_attach_to_party` 1660) refuses an attachment that would close a cycle; spawning past
`kMaxSpawnedAgents` / `kMaxSpawnedMissionObjects` (8192) returns -1. A faded-out scene prop is never
removed (`scene_prop_fade_in` 1823 must be able to restore it). Party keys are capped at
`kMaxPartyKeys` (`131072`): every key below `next_party_key` costs a `WseParty` in the dense
vector, so saves and `parties.txt` above the cap are rejected (`count_limit_exceeded`) and
`allocate_party` (`spawn_around_party` 1100, 1101) returns -1 there. The window queue is capped at
`kMaxQueuedWindows` (`64`, oldest dropped), and the campaign consumer pops window types that only
unmodelled windows (menu, inventory, party screens) would consume, leaving the active window
unknown (`-1`) instead of leaving them queued forever. Open questions are in `STATUS.md` §5.

### Scene objects and skinning (Phase 2A / P9)

`instantiate_scene_objects` (`wse/game/scene_loaders.h`) replaces the mission entities through
`clear_mission_entities`, so nothing keeps a reference to the previous scene's object ids. Each
`.sco` record keeps its kind (`0` prop, `1` entry point, `2` item, `4` flora, `5` passage) in
`meta_mesh_no`, the meta type `try_for_prop_instances` filters on; only props, passages, and items
get a `scene_prop_no`, so an entry point or flora record never counts as a scene prop of the same
number. Entry points also fill `world.entry_points`. It returns how many records did not fit in
`kMaxSpawnedMissionObjects`.

Safety deviations in `wse/asset/skeleton.h`: `build_mesh_skin_data` keeps a vertex's 4 largest
bone weights (not the first 4 in group order), and `skin_brf_mesh` divides by the kept weight sum,
so weights that do not add up to 1 do not pull vertices toward the bone-space origin.

### UI runtime, bitmap fonts & RHI widgets (P9 Part 2)

- **UV orientation (`wse/asset/font.h`, `wse/platform/rhi_ui.h`):** `rglFont` (`0x140297a20`, `0x14029b7d0`) and Warband UI `.brf` meshes store bottom-up `uv.v` (`v = 1` at the top of the texture, `v = 0` at the bottom), while `decode_dds_mip_rgba8` and `SoftwareRhiDevice::sample_texture` index top-down rows (`v = 0` at row 0). `FontData` preserves retail bottom-up UV math, and `UiRhiRenderer` flips `v_rhi = 1.0f - v` when uploading BRF UI meshes and glyph quads.
- **Modal window closing (`wse/game/ui_runtime.h`):** `UiSession::close_active_modal_window` closes `0x15` (`mbNotesWindow` `0x1401616a0`) and `0x16` (`mbPresentationWindow` `0x140175050`) back to `source_window` (`mbGameScreen::setWindow` `0x1400f0de0`) so `3039`/`3044`/`4901`/`4907` are unblocked. Closing a presentation resets `active_presentation_no = -1` and drops queued `0x0f` entries for that presentation only (a queued different presentation stays); closing notes drops queued `0x10` entries. The modal widget tree is replaced by an empty container, and the call is a no-op when no modal window is open. `open_notes_window` / `start_presentation` default `prev_window` to the window that was current. `sync_overlays_from_world` preserves per-overlay `highlighted`/`pressed` state across trigger-driven overlay resyncs.
- **Presentation runtime (`wse/game/ui_runtime.h`):** triggers `-61` / `-62` / `-63` get their parameters in `world.misc.trigger_params` (read by `store_trigger_param_N` 2070..2073; saved and restored around the call), never in `reg0`/`reg1`: `-61` param 1 = elapsed ms, `-62` param 1 = overlay, param 2 = overlay value, `-63` param 1 = overlay, param 2 = 0 enter / 1 leave. `start_presentation` clears `world.overlays` / `m6.ui.overlay_ext` so overlay ids restart at 0, arms `world.presentation_duration` (1/100 s; 10 s for read-only flag bit 0 per `0x140209a47`, else `default_duration_s`), and keeps one mutable copy of the presentation's triggers for its lifetime. Each `step_presentation` counts `presentation_duration` down; the window closes when it reaches 0, so `presentation_set_duration` (902) `0` closes and `999999` keeps it open. A `start_presentation` (900) issued by a running presentation's trigger switches to the new presentation (firing its `-60`, consuming the queued `0x0f` entry; at most 8 switches per call). Naming the open presentation restarts it (overlays cleared, duration re-armed, `-60` fired again), in or out of a scene: 900 bumps the port-only counter `m6.ui.presentation_start_serial`, so the Native refresh idiom `presentation_set_duration 0` + `start_presentation <self>` (12 of Native's 49 presentations, e.g. `prsnt_game_custom_battle_designer`, `prsnt_multiplayer_item_select`) refreshes instead of closing. A request still pending after the 8-switch bound is dropped, not replayed next frame. Escape does not close a presentation: retail `mbPresentationWindow::frameMove` (`0x140175050`) has no key check and modules poll `key_clicked` in their own `-61` trigger (17 of Native's 49 presentations do). `prsntf_manual_end_only` (`0x2`) is tested only by `mbConversationWindow::updatePresentations` (`0x140088a20`), which keeps such a presentation when another one queued by `mbConversationWindow::startPresentation` (`0x140088a00`) starts; the port records the constant for the mission-side presentations it does not drive yet.
- **Frame driver (`wse/game/ui_runtime.h`, port infrastructure):** `UiSession::drive_frame` runs after `WseWorldState::step_world_frame` and is called by `run_headless_module` every frame while `HeadlessRunOptions::drive_ui` is set (on the caller's `UiSession`, `run_headless_session`'s own, or else a run-local one, so `wse_headless` drives it too). On campaign frames, a window `0x16` opened by `consume_campaign_queued_window` starts `active_presentation_no` (no `-61` on the opening frame; an unknown number puts the previous window back), a window `0x15` opens the notes window for `m5.screen_note_kind` / `screen_note_object`, and otherwise an open presentation steps. The return window is the one up before the queue was consumed, or, when opened over another modal window, wherever that one returns to; notes opened over a presentation close it first. On mission frames (`lighting.has_scene`) an open modal window is dropped without writing a campaign window back. **Port approximation:** the notes text is the object's name / title, then its description (quest, info page) and non-empty note slots, one per paragraph. **Headless campaign state:** `run_headless_module` clears `lighting.has_scene` after loading (`start_on_campaign_map`), since no mission scene is loaded and the struct default `true` would make every frame a mission frame.
- **Overlay widgets (`wse/game/ui_runtime.h`):** overlay types follow the create opcodes: `0` mesh, `1` text (`create_text_overlay` 910, `tf_*` flags in `val`) or text button (912/940/941, `m5.is_button`, flags in `widget_flags`), `4` image button / check box. Only text buttons and type-4 overlays are clickable; flags `0` keep left alignment. Colours are `0xRRGGBB` (`overlay_set_color` 921) and the alpha byte comes from `overlay_set_alpha` (922) / `m5.highlight_alpha`, replacing (not multiplying) any alpha in the colour. **Overlay sizes (retail-checked):** `overlay_set_size` calls the widget's `setScale`. Text: `rglWidget::setScale` (`0x14031f5a0`) sets the text size to `0.02 * size`. Mesh overlays: `rglImageWidget::setScale` (`0x1402a6e40`) rescales the mesh by `new / old`, so they span their mesh's UI-space bounds (`UiSession::mesh_bounds`, from `platform::collect_ui_mesh_bounds`) times the size. Text buttons without `overlay_set_area_size`, by create opcode: `create_button_overlay` (912) sets text flag `0x200`, so `rglWidget::setText` (`0x14031f0f0`) makes the box the laid-out text extent at the text size of the moment (0.02 at creation); `UiSession` measures it with `UiSession::font` and keeps that box when `overlay_set_size` later scales the text (no font: fixed `0.25 x 0.038 * size` fallback). `create_game_button_overlay` (940) / `create_in_game_button_overlay` (941) build an `rglGameButtonWidget` with fixed centred text (flags `0x10`), `medium_button(_down)` / `mp_inventory_button` meshes, and a `0.148 x 0.04` / `0.2123 x 0.04` box; `rglGameButtonWidget::setScale` (`0x14029c110`) makes the `overlay_set_size` vector the box and leaves the text size alone. It also multiplies the game-button meshes by new / stored scale per axis: `createGameButtonOverlay` (`0x14020c560`) scales the `medium_button` meshes uniformly by 5/9 (`0x3f0e38e4`) and stores (0.148, 0.04); `createIngameButtonOverlay` (`0x14020c950`) keeps `mp_inventory_button` at native size and stores (5.3075, 1.0) (`0x40a9d70a`). The meshes are drawn through `UiWidget::has_mesh_rect` / `mesh_pos` / `mesh_size` (`rhi_ui.h`), and the box stays the hit area. Creation highlight text colours are `0xff007700` (940) and `0xff00ff00` (941). **Port approximations:** a 912 box is measured at 0.02 at creation, and `overlay_set_text` (920) re-measures it at the `overlay_set_size` scale in force (`m5.text_measure_scale`), as retail `setOverlayText` (`0x14020e110`) -> `rglWidget::setText` (`0x14031f0f0`) does. Overlay text without an area is anchored at the overlay position (first baseline on it), aligned around it, and broken only at newlines (not yet checked against retail).
- **Interactive overlays (`wse/game/ui_runtime.h`, `wse/platform/rhi_ui.h`):** `UiSession::handle_input(world, UiPointerInput, ...)` is the pointer API (position, left/right/middle press edges, left release, wheel notches, monotonic `time_s`); `handle_pointer_and_keys` stays as a tap wrapper (press and release in one frame), and the built-in windows now act on release over the pressed button. Routing follows `rglWindowManager::frameMove` (`0x14032ae90`: press -> states `0x10` / `0x40`, release -> `0x8` / `0x20`, to the focused widget else the window tree) and `rglContainerWidget::triggerState` (`0x14028b090`: hover `0x2` / `0x4` to every visible, enabled child entered or left; presses top-down until one consumes; disabled or hidden children skipped). `rglWidget::triggerState` (`0x14031ead0`) latches states with `setState` (`0x14031dee0`); `checkState` (`0x14031dec0`) tests and clears. `mbPresentationContainer::frameMove` (`0x140209040`) fires, per overlay in index order: `0x2` -> `-63` (0), `0x4` -> `-63` (1), `0x10000` -> `-62` (value by `getType`: types 2/3 float value truncated, 5/11/12/14 int value, 6/7 text copied to `s0`, 9/10 toggle state when the toggle flag is set), then `0x10` -> `-64` (0), `0x40` -> `-64` (1), `0x100` -> `-64` (2). Retail `getType`: slider 2, combo button 5, text box 6, simple text box 7, scrollable 8, image button 9, game button 10, number box 11, list box 12, table 13, combo label 14. Widgets: plain buttons arm on press, disarm on leave and raise `0x10000` on release while armed; toggle buttons (flag `0x284` bit 0) flip on press and raise `0x10000`; both consume left presses (`rglSimpleButtonWidget::triggerState` `0x140308530`); check boxes are image buttons plus the toggle flag (`createCheckBoxOverlay` `0x14020bdd0`). Sliders: 0.25 wide, knob margin 0.008, horizontal, start at min (`createSliderOverlay` `0x14020b0b0`); a press within margin + 0.01 of the knob grabs it, else jumps (`rglSliderWidget::triggerState` `0x14030e3d0`, `setAbsolutePosition` `0x14030e820`); while dragging every move sets the value and raises `0x10000`. Number boxes: arrow press steps 1, hold repeats after 0.5 s every 0.1 s with step 1 / 3 / 10 / 50 (up to 2.5 s / 5 s / 7.5 s / beyond), clamped (`rglNumberBoxWidget::triggerState` `0x1402c8580`). Combo labels: arrows step +-1 with wrap over the item count and always raise `0x10000` (`rglComboLabelWidget::triggerState` `0x140262e50`, `setIntValue` `0x140262df0`); combo buttons open the popup on press when they have items (`rglComboButtonWidget::triggerState` `0x1402614c0`). `create_text_overlay` with text flag bits 17-19 (`0xE0000`) makes a scrollable (`createTextOverlay` `0x140209c10`, `rglScrollableWidget::getType` `0x140301080` = 8) and the wheel scrolls the one nearest the cursor. `UiWidget` children render at `child_offset` and children wholly outside a clipping container are culled. The overlay create handlers record `m5.create_opcode`. **Bug fix:** `create_text_overlay` (910) now joins the container set by `set_container_overlay` like the other create ops and records create opcode 910 (it previously ignored the current container). **Port approximations:** number-box / combo-label arrows are the outer quarters of a fixed box (`kUiNumberBoxWidth` 0.12 / `kUiComboWidth` 0.25, height 0.04); the combo popup lists items downwards (`rglComboButtonWidget::openPopup` `0x140261e90` not traced); list boxes behave like combo buttons; the slider grab offset is the cursor-to-knob distance; scroll step 0.05 per notch, initial scroll at content top; no scissor, so clipping culls only children wholly outside; for plain buttons retail's `-62` param 2 is a stale trigger parameter and the port passes the overlay value; triggers run at input time, not after `-61`; text boxes are approximated (see the overlay animation and text box note below); slider / number box / combo meshes are not drawn (text only); the middle-press (`0x100`) sender is untraced; area-less text overlays have no hover box. **Safety deviation:** the slider float-to-int value conversion saturates instead of invoking undefined behaviour on out-of-range values.
- **Menus (`wse/game/ui_runtime.h`):** with a `VmContext`, `build_menu_window` runs the menu's operations block when it opens (`mbMenuWindow::open` `0x1401203a0`) and formats the menu and option text through `wse_parse_string` (`{sN}`, `{regN}`, `^`). **Driving `0x12`:** `UiSession::drive_frame` (last parameter: the module menus) builds the menu for `world.jump_menu_no` (set by `jump_to_menu` 2060, `0x1401dc431`) when `consume_campaign_queued_window` opens `0x12`, after closing any open notes or presentation window. The port counter `world.m6.ui.menu_open_serial` (bumped per consumed `0x12`) makes a menu-to-menu jump rebuild. The menu number and the `world.ui.current_menu_no` write are port choices. `run_headless_module` passes `bundle.menus`.
- **Fonts (`wse/asset/font.h`):** safety deviation: `font_data.xml` with `font_scale <= 0` is rejected (it would zero every layout scale and divide by zero at tab stops). `layout_text` with no box (`max_x <= min_x`) does not word-wrap but still breaks at `'\n'`; `TextLayoutResult::width` is the widest line's own width, so `measure_text` is alignment-independent.
- **Number boxes and combo popups (`wse/game/ui_runtime.h`):** a number box holds a numeric text box (`rglNumberBoxWidget::updateWidgets` `0x1402c88e0`). A press between its arrows focuses it; a typed digit makes the value `strtol(text) * 10 + digit` with the minimum forced to 0, backspace erases a character, other characters are ignored, and Enter / Escape / a press outside (an arrow included) commits through `setIntValue` and fires `-62` (`rglTextBoxWidget::triggerState` `0x140317990`). `set_number_box_value` follows `rglTextBoxWidget::setIntValue` (`0x1403177d0`): the value clamps to `[min, max - 1]` (**bug fix:** arrows used to reach `max`, which the module header excludes), and an empty range gives -1 with empty text. While focused the box shows the typed text. The combo popup's items are as tall as the combo box (`rglComboButtonWidget::openPopup` `0x140261e90`; was a fixed 0.035). Placeholder text boxes (`+0x292`) exist only in engine windows, never module overlays. **Safety deviation:** a typed value saturates instead of wrapping in 32 bits before the clamp. **Port approximations:** the text box is the middle half of the number box; popups that would leave the screen are not moved or made scrollable.
- **Shader warm-up (`wse/asset/shader.h`, port infrastructure):** `SpirvShaderCache::warm_up(vfs, dir, requests, jobs)` probes every request on the calling thread, compiles the distinct misses on up to `jobs` threads sharing `compiler_backend`, then stores the modules and updates `cache_hits` / `compile_count` / `last_error` on the calling thread, so neither the VFS nor the cache object is touched concurrently. `WarmupReport` lists hits, compiles and per-request failures. `get_or_compile` now shares the same `load_cached` / `store_compiled` halves.
- **Overlay animation and text boxes (`wse/game/ui_runtime.h`, `wse/platform/rhi_ui.h`):** opcodes `932`-`937` (colour, alpha, scale, position targets) capture the current overlay value as the start and set the `WseOverlayM5Ext::anim_active` bits `kOverlayAnim*` with `anim_*_start` and `anim_elapsed` (`native_ui_start_anim`), as retail `rglWidget::animateToColor` / `animateToAlpha` / `animateToScale` / `animateToPosition` (`0x14031d980` / `0x14031d9c0` / `0x14031da80` / `0x14031dac0`) do. `UiSession::step_presentation` interpolates per retail `rglWidget::frameMove` (`0x14031ced0`): `(start*(d-t) + target*t)/d`, with colour bytes and alpha rounded half-up (floor, +1 if the fraction is above 0.5), size and position unrounded, and the target set exactly when time is up. Time is the presentation frame time. A left press focuses a text box (`rglTextBoxWidget::triggerState` `0x140317990`, `obtainFocus` `0x140318010`). While it has focus it gets every press and every character from `UiPointerInput::text_input` (printable characters appended, `<` and `>` rejected, backspace removes one character). Enter, Escape, or a left press outside commit via `rglTextBoxWidget::loseFocus` (`0x140317fe0`, raises `0x10000`), and `-62` then fires with the text in string register 0; `mbPresentationContainer::frameMove` (`0x140209040`) copies it into `mbBasicGame::m_stringRegisters` (`+0x2670`, PDB layout). **Port approximations:** no maximum length (retail field `+0x294` not traced), no numeric mode (`+0x298`), no placeholder clearing (`+0x292`), no IME composition; `overlay_obtain_focus` (949) does not commit a previously focused box.
- **Mission presentations (`wse/game/ui_runtime.h`):** `UiSession::update_mission_presentations` runs once per mission frame (`world.lighting.has_scene`) and ports `mbConversationWindow::updatePresentations` (`0x140088a20`). Pending starts from `start_conversation_presentation` (4907) come from `m6.ui.pending_conversation_presentations`; each one ends the open containers that show the same presentation or lack `prsntf_manual_end_only` (flag bit `0x2`), then opens a container with duration `10 s` for read-only presentations (flag bit `0x1`) and `100 s` otherwise (`mbPresentationContainer::startPresentation` `0x140209930`) and runs `-60`. Every container runs `-61` with its elapsed milliseconds and closes when its duration runs out. The mission branches of `presentation_set_duration` (902) and `is_presentation_active` (903) use `m6.ui.mission_duration_requests` and `m6.ui.active_presentations`. The campaign presentation state (`active_presentation_no`, `presentation_duration`, and the trigger manager) is saved and restored around the mission pass. Leaving the scene clears the stack and both queues; a mission frame also closes an open notes or presentation window. Native `900` has no mission branch (`0x1401cc082` only queues `0x0f`). Each container keeps its own overlay tree and interactive state (hover, armed button, drag, number-box hold, combo popup, scroll, text-box focus); while any container is open `UiSession::root` draws them in stack order (later on top). `handle_input` routes the pointer to them top container first: hover enter / leave reach every container, and a press one container consumes is not handed to those below (`rglContainerWidget::triggerState` `0x14028b090`). A container's triggers run with its own number; a `start_presentation` (900) inside one cannot renumber it. **Port approximations:** a pressed container is not raised to the top; triggers a container fires at input time run before that frame's `-61`.
- **Frame glue (`wse/platform/ui_frame.h`, port infrastructure):** `UiInputAccumulator` turns one frame of `RawInputEvent`s into a `UiPointerInput` in Warband UI space (`x` in `[0, 1]`, `y` up in `[0, 0.75]`): mouse motion and buttons, the first touch finger as the left button (a cancelled touch releases without a click), the wheel, and Escape / Backspace / Enter, which reach a focused text box as `\x1b`, `\b`, and `\r`; `feed_text` takes the host's text events. `run_ui_frame` runs `UiSession::handle_input`, then `drive_frame`, then `UiRhiRenderer::render_tree`.
- **App shell (`wse/platform/ui_game_shell.h`, port infrastructure):** `UiGameShell::start` loads `load_native_ui_assets` (the font is required), a `SoftwareRhiDevice` + `UiRhiRenderer`, and the module through `game::HeadlessModuleRunner`, then shows the main menu (`build_initial_window`, module label from the last `module_dir` component; saves, tutorial and multiplayer hidden). Main-menu frames run input only, no module frames. "Start a New Game" runs `game_start` (`run_game_start`); if it queued no window, game menu 0 is queued as `jump_to_menu` does (port choice, see `STATUS.md` §5 P9 Part 2 item 1); the quit button sets `quit_requested()`. `frame(dt, w, h)` clamps `dt` to `[0, max_frame_seconds]` (0.25 s, so a return from the background is not one huge step), steps the module once with that frame's pointer input, and renders at the window's aspect ratio with the height capped at `max_render_height` (720). Unimplemented opcodes are counted, not fatal, by default. `start_at_initial_window = false` runs `game_start` in `start` instead (tests). Before loading, `start` runs `find_missing_warband_files` (module folder, its `module.ini` / `scripts.txt` / `troops.txt` / `menus.txt`, `Data/font_data.xml` in the module or the install, and `CommonRes/`) and refuses with one message listing everything missing (`format_missing_warband_files`, `missing_files()`); `check_install = false` skips it for synthetic test modules. The SDL host shows that message (or any other start error) in `SDL_ShowSimpleMessageBox` and returns to the launcher; module mode is landscape-only (`SDL_HINT_ORIENTATIONS` plus `WseActivity`'s sensor-landscape request before SDL starts). Frame reuse: `UiTreeHasher` (FNV-1a over every `UiWidget` field, recursively) hashes the tree each frame; with the same hash and frame size the rasterizer is skipped (`frames_reused()`), and `frame_serial()` changes only on a redraw. `render_tree` reads nothing but the tree, the font, the config and the uploaded assets, so this is exact; a field added to `UiWidget` must be added to the hasher.
- **Stepped runner (`wse/game/headless_runner.h`):** `HeadlessModuleRunner` splits `run_headless_module` into `start` (load, audio, Lua, optional `game_start`), `run_game_start`, and `step(frame_ticks, input, events)`; `run_headless_module` is `start` plus a `step` loop, so `wse_headless` results are unchanged. `step` hands `input` to `UiSession::handle_input` after the world frame and before `drive_frame`, the order `run_ui_frame` uses.
- **SDL host (`platform/sdl/sdl_selftest_main.cpp` `run_module_shell`):** `--module <warband_root> [module_rel_dir]` (Android: `adb shell am start -n org.wse2.android/.WseActivity -e wse.module_root <dir> [-e wse.module Modules/<name>]`) loads through `HostVfs` with Lua off, feeds `SdlInputBackend::poll_events` and `SDL_EVENT_TEXT_INPUT` into the shell, and streams `pixels()` into an `SDL_PIXELFORMAT_RGBA32` texture stretched over the window (Warband's UI space is stretched too, so window-normalised input maps directly). A stopped module keeps its last frame with the error on top. The texture is re-uploaded only when `frame_serial()` changes, after `SDL_EVENT_RENDER_TARGETS_RESET` / `SDL_EVENT_DID_ENTER_FOREGROUND`, and recreated after `SDL_EVENT_RENDER_DEVICE_RESET`. `--saf-module <module_rel_dir>` (Android) loads from the tree `WseActivity` registered instead.
- **P10 launcher and start-up support (port infrastructure, no retail evidence):**
  - `ui_game_shell.h`: `install_check_json` (the launcher's per-module check, JSON-escaped by `json_escape`), `list_warband_languages` (`en` first, then valid `languages/*/` folder names sorted case-insensitively). `start_async` runs the same loader as `start` on a worker thread (`LoadWork` built off-thread, committed on the host thread by `poll_start` after the join); `start_progress`, `cancel_start` (checked between stages, error "Start cancelled"), `worker_thread_init/exit` hooks; while loading every host-thread accessor is inert; `stop()` and the destructor cancel and join. `max_texture_size` caps every UI texture except the font atlas; `texture_bytes()` reports decoded texture memory.
  - `performance.h`: `QualityPreset` (Low 30 FPS / 0.75 / 512, Medium 30 / 0.85 / 1024, High 60 / 1.0 / 2048, Custom), `UiAspectMode`, `render_scale` [0.5, 1], `max_texture_size` (0 or a power of two in [256, 4096], rounded down), `ui_scale` [0.75, 2], `language` (1-16 chars of `[A-Za-z0-9_-]`, else `en`); `ensure_first_launch_settings` (missing or corrupt file -> Medium preset, saved); `save_launcher_settings_json` overlays a partial JSON object (preset first, explicit keys win, any invalid value writes nothing).
  - `dds.h`: `select_budget_mip` (largest mip whose sides fit; 0 = no cap) and `decode_dds_rgba8_budgeted` (decodes only that mip, then 2x2 box-halves while a side is still over the cap, rounding odd sizes down like mip chains).
  - `platform/android/perf_hint.{h,cpp}`: ADPF `APerformanceHint_*` loaded with `dlsym` (API 33+), work time reported per module frame, `setpriority(-4)` fallback; `RegisteredGameTree::rebind_thread` moves the SAF tree between JNI-attached threads.
  - `sdl_selftest_main.cpp`: `PerfLoopState` (launch config, thermal step-down, display frame rate) shared by the self-test loop and `run_module_shell`; loading screen at <= 30 FPS; `--show-fps` overlay (updated twice a second).
- **GPU UI path (`wse/platform/rhi_geometry.h`, port infrastructure):** `GeometryCaptureRhiDevice` is an `RhiDevice` whose render pass records screen-space triangle batches (`GeometryFrame`: `GeometryVertex` laid out like `SDL_Vertex`, batches of consecutive draws with the same texture and blend mode) instead of rasterizing; it leaves the readback empty. It follows `SoftwareRhiDevice` exactly for the clip-to-pixel mapping (row 0 at NDC `y = -1`), texture orientation, per-draw tint, vertex colour, cull test and D3D9 half-pixel shift, and transforms only the vertices a draw references. Not expressed (counted in `unsupported_draws()`): depth test, alpha test, lighting, normal / specular maps, sRGB, and near-plane clipping (`w <= 1e-6` triangles are dropped). `UiGameShellOptions::Output::geometry` uses it at the window's full size; `SdlGeometryPresenter` (`sdl_selftest_main.cpp`) creates SDL textures from `texture()` on first use, frees them from `take_destroyed_textures()`, maps blend modes (`alpha` -> `SDL_BLENDMODE_BLEND`, `additive_alpha` -> `ADD`, `multiply` -> `MOD`, `additive` -> custom ONE/ONE), and calls `SDL_RenderGeometry` per batch with linear filtering (the software path samples nearest). The app presents only when the frame serial, window size or stopped state changed, or after a render reset, a window expose, or a return to the foreground.
- **P11 in-game UI and touch overlay (port infrastructure, no retail evidence):**
  - `ui_frame.h`: `compute_ui_viewport` (largest centred rectangle of the mode's ratio, `llround`, offsets by integer halving; `Stretch` = whole window; empty window = zeros). `UiInputAccumulator::set_viewport` maps window-normalised points through the viewport without clamping, so a tap on a letterbox bar lands outside the UI canvas and hits nothing; it re-maps an already-buffered position.
  - `rhi_geometry.h`: `GeometryCaptureRhiDevice::set_viewport` offsets and scales vertices into the viewport while `GeometryFrame::width/height` stay the pass size (zero size resets). `UiGameShell::frame` computes the viewport from `UiGameShellOptions::ui_aspect` (default 4:3), feeds it to the input accumulator, and includes it in the frame-reuse key; the software path renders the viewport size (capped by `max_render_height`).
  - `input.h`: widget sizes are fractions of the short side; `apply_safe_area` clamps every rect inside normalised insets; new `TouchWidgetId` entries are appended so saved profiles keep their order, and old profiles load with defaults. Attack and block classify their direction once the finger passes `swipe_threshold` or 120 ms pass, whichever is first; a short tap attacks in the default direction. The order radial is two-level (group: all / infantry / archers / cavalry, then order); in one continuous drag, leaving ring 1 commits the group last hovered inside it.
  - `touch_overlay_render.h`: untextured triangle batches (fans for circles, rounded rects, arrow glyphs) for the widgets visible in a context, per-widget opacity, pressed state brighter, inside the safe area. `UiGameShell::set_touch_overlay` appends them after the UI batches in geometry output; the overlay state is part of the frame-reuse key.
  - `UiGameShell::step_input(mgr, events, dt)` is the app's input path (replaces `feed()`): it sets `mgr`'s context from `world.m5.active_window` (`WorldInputBridge::context_for_screen_window`), so the overlay shows only in Mission / CampaignMap / Dialog / Party / Trade, drops touches that start on the visible overlay (`WseInputManager::overlay_claims_touch`, same rule as `handle_touch_down`) from the menu UI until that finger lifts, runs `mgr.process_frame`, and mirrors keys into `world.input` with `WorldInputBridge`. Deviation: `world.m5.cursor_pos` keeps the UI's letterbox-mapped cursor instead of the manager's window-normalized one. The SDL app clamps a copy of the user's layout into `SDL_GetWindowSafeArea` (as normalized insets) on every window or safe-area change and stores it in `mgr`, so drawn and hit-tested rectangles match.
- **P11.2 / P11.5 / P11.6 (port infrastructure):**
  - `rhi_geometry.h`: each textured draw is classified from its first triangle: axis-aligned (1e-3) with both texel-to-pixel ratios in 1 +/- 0.02 -> `GeometryFilter::nearest`, else `linear`; filter and clip rectangle are part of the batch key.
  - `rhi_ui.h`: `UiRenderConfig::text_scale` ([0.75, 2], non-finite -> 1) scales glyph size and advance around the alignment anchor; widget boxes and hit areas keep the module's layout, so single-line labels can draw past their box above 1.0. Draws inside `clip_children` containers carry the intersected clip (`MeshDrawCall::has_clip`, UI virtual units); partly visible children are drawn and clipped. `SoftwareRhiDevice` discards pixels outside the clip; `VulkanRhiDevice` does not apply it yet.
  - `ui_runtime.h`: with `UiPointerInput::touch`, a vertical drag past 0.01 UI units inside a scrollable container scrolls it and cancels the armed press; a press on a widget not hovered the frame before still clicks. Mouse input is unchanged. Closing a presentation clears the focused overlay (deviation: keeps the soft keyboard from staying open for a box that no longer exists). `UiGameShell::text_input_wanted` reports a focused text or number box.
- **P12.2 module art (`wse/asset/bmp.h`, port infrastructure):** `decode_bmp` reads `BI_RGB` 24/32-bit, 8/4-bit palettized and `BI_BITFIELDS` 32-bit, bottom-up or top-down, every read bounds-checked; it rejects RLE, sides of 0 or over 8192, truncated data and out-of-range palette indices. `downscale_to_fit` box-averages to a max side; `seed_color_argb` takes the heaviest of 36 hue bins weighted by saturation x value (pixels below 0.2 saturation or 0.15 value ignored; `0xFF5B6B8C` when none qualify). `nativeLoadModuleArt` reads `<module>/main.bmp` through the cached SAF tree.
- **P4 GPU rendering (`platform/vulkan/driver_loader.cpp`):** `VulkanRhiDevice` renders on the GPU when the device passes an execute check (queue, command pool, a trivial submit and fence); the mock ICD fails it and keeps the CPU reference path (`last_frame_used_gpu()` reports which ran). It allocates device-local and host-visible staging memory from `vkGetPhysicalDeviceMemoryProperties`, uploads buffers and RGBA8 textures through staging, compiles the built-in material shaders once through `wse_shader`, caches pipelines by `PipelineDesc`, flips Y and remaps depth to match the software frame and D3D9 depth, and reads the offscreen colour target back top-down. Real-GPU frames are compared to `SoftwareRhiDevice` within 8/255 per channel, not by hash.
- **SAF game tree (`platform/android/saf_game_tree.h`):** the launcher's SAF cache binds a JNI session per call on the UI thread, which the SDL thread cannot use. `WseSafBridge.nativeRegisterGameTree` keeps a global reference to a bridge for the picked tree; `RegisteredGameTree::open(SDL_GetAndroidJNIEnv())` binds a `ScopedBridgeSession` to the calling thread for the tree's lifetime, indexes it (`SafVfs::rebuild_index`), and must only be used on that thread.
- **Text boxes and sliders (`wse/game/ui_runtime.h`, `wse/vm/native_ui_ops.h`):** a text box holds at most 28 code points (`kUiTextBoxMaxLength`, `rglTextBoxWidget` constructor `0x1403176c0`, `+0x294`). `overlay_obtain_focus` (949) commits the previously focused text box (`rglTextBoxWidget::loseFocus` `0x140317fe0`) through `m5.commit_pending`, and the next `handle_input` raises its `-62`. Sliders draw the `slider_hor` track and a `handle_hor` knob at the value's position (`createSliderOverlay` `0x14020b0b0`, `rglSliderWidget::getPercentage` `0x14030e8a0`); the track height comes from the `slider_hor` mesh bounds, with a fallback when they are missing. Not drawn yet: number-box and combo meshes, numeric text boxes, and IME composition.
- **UI assets (`wse/platform/rhi_ui.h`):** `load_native_ui_assets` keeps meshes from the core UI resources (and the CommonRes `user_interface_b/c` packs only when `module.ini` does not load them), plus core or module meshes named by `kUiSessionMeshNames`, `extra_mesh_names`, or an already-indexed core UI mesh; module resources are applied last so their overrides win. Other meshes (3D content) are not copied, decoded, or uploaded. `UiWidget::material_name` resolves through the material's diffuse texture. Mesh overlays and image buttons set `UiWidget::tint_mesh`, so `color_argb` (overlay colour + alpha) reaches the draw as `MeshDrawCall::tint_rgba`, which the software rasterizer multiplies into every vertex colour (retail `rglImageWidget::setColor` / `setAlpha` write the mesh colour; game buttons do not tint their meshes). `upload_texture` creates the replacement texture before destroying the old one.

- **RHI buffer updates (`wse/platform/rhi.h`, `rhi_ui.h`, `rhi_scene.h`, `platform/vulkan/driver_loader.cpp`):** `RhiDevice::update_buffer(handle, data, error_out)` replaces a live buffer's contents. `SoftwareRhiDevice` writes it in place; `VulkanRhiDevice` re-creates its mirrored VkBuffer only when the data grows (real GPU upload and sync semantics wait for the P4 pipeline). `MeshDrawCall::first_index` lets draws share one index buffer. `UiRhiRenderer::render_tree` keeps one dynamic vertex buffer and one index buffer across frames, refilled with `update_buffer`, and each text or panel batch draws from its `first_index`. `BrfSceneRenderer::update_skinned_mesh` refills its vertex buffer, and its index buffer when the source mesh changes, in place.

## Savegames

`rglBinaryStream` stores raw little-endian values with no header or per-value metadata;
`BinaryChecksumWriter` (`rglOutputBinaryFileStreamWithChecksum`) mirrors retail byte-sum tracking
modulo `2^64`. Trigger savegames are an `int32` count, then per manager trigger `int32` status
plus `int64` check, delay, and rearm timer values; simple triggers store one `int64` timer value.
Whole-savegame framing (`wse/game/savegame.h`) wraps `"WRDR"` magic (`0x52445257`), container
version (`[802..1174]`), module version (`>= compatible_savegame_module_version`), `mbSavegame`
header (`fill_savegame_header` / `read_savegame_header`), and all `mbGame` section codecs
(`save_world_to_vfs` -> `new_game.sav` -> read-back checksum -> `last_savegame_backup.sav`). Note:
`ctx.world->troops` must already hold module troop hero flags before loading (`loadParties`
`0x1400c9420`). `read_trigger_savegame`/`write_trigger_savegame`/`read_simple_trigger_savegame` and
`read_savegame_container` use explicit status enums (`SavegameResult` / `SavegameContainerResult`)
to validate counts/bounds instead of reading past buffers like retail.

Safety deviations and portable choices in `savegame.h` (each keeps valid saves loading unchanged):

- **Transactional load.** The body is parsed into copies of the world, global variables, and both
  trigger managers and committed only when everything parsed; retail mutates `mbGame` as it reads.
- **Allocation bounds.** Every section count is checked against the bytes left (at least 4 bytes
  per party / event slot, 28 per quest, 208 per info page) before anything is allocated.
- **Corrupt records.** A `rglHashVector` / `rglStableVector` present flag other than 0 or 1 returns
  `corrupt_record`; a party key outside the slot table is consumed, dropped, and sets
  `count_exceeded`.
- **Versioned bodies (`[802..1174]`).** `read_world_body_sections` and `read_party_record` (and
  optionally `write_savegame_container`) implement the four `g_compatibleSavegameVersion` branches
  verified in v1.1.5.6 (`is_marshall` `(900..999) || >= 1020`, `extra_icon_*` & 5 combat options
  `> 1136`, `ignore_with_player_party` `> 1160`), with omitted fields reset to retail defaults
  (`SavegameContainerResult::legacy_layout_unsupported` remains in the API enum).
- **Troop merge.** Loaded troop fields are merged into the module troop; when
  `bDontLoadRegularTroopInventories` skips a regular troop's gold / xp / HP / faction / inventory /
  face / name / class block, the live values stay.
- **Inventory amount.** Each slot stores `ammo` (the retail item amount) and the modifier; the
  portable `hit_points` field has no retail slot counterpart and reloads as 0 (`set_item`).
- **Party icon.** `icon_no` is folded into `flags & 0xff` on save (so `party_set_icon` persists) and
  re-derived from it on load, as the module loader does.
- **Float casts.** `ignore_player_timer` saturates into the int64 timer (NaN / negative -> 0) and a
  non-finite bandit attraction loads as 0, instead of undefined float-to-int conversions.
- **Read-back.** `save_world_to_vfs` requires the read-back to match length and bytes as well as the
  byte sum, which cannot see lost zero bytes.

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
The register bank is the 128-entry int64 array at `g_basicGame+0x270` (`0x140663c60`), the same bank
that operand tag 1 addresses; `register_get` writes the indexed value through operand 0, and
`register_set` stores operand 1 into the register named by operand 0 and returns without a store.
Both reject an out-of-range index with `WseDispatch::ignored`, matching `mbCheckRegister`
(`0x1401c4fe0`) and the retail `return true` fall-through. `store_wse_version` ignores its value
operand: selector 0 yields the WSE version 5, selector 1 yields the Warband build `0x483` (1155), and
any other selector yields the zeroed default (`0x1401e56ba`–`0x1401e56db`).

The script-context, timer, RNG, and profiler group covers `store_current_trigger` (3014),
`return_values` (3015), `store_num_return_values` (3016), `store_return_value` (3017),
`mtsrand`/`mtrand` (3020–3021, `rglMersenneTwister`), `get_time` (3022, UTC/local 32-bit epoch),
`timer_reset`/`timer_get_elapsed_time` (3027–3028, 128 slots tracking 100 kHz `timers_total` with `op1 != 0 ? 0 : 2`),
`set_random_seed` (3034), `store_application_time` (3035), `profiler_{start,stop,is_recording,mark}`
(3040–3043), and `store_cur_mission_template_no` (3600). `store_current_trigger` returns the statement's context
argument only while it is negative (the in-trigger marker; outside a trigger it is 0), matching
`WSEOperationContext::GetCurrentTrigger`. `return_values` writes up to 16 `int64_t` slots at
`0x140699b20`/`0x140699b18`, `store_num_return_values` reads the count at
`0x140699b18`, and `store_return_value` reads a 1-based slot (out-of-range/unset logs and returns true without storing).
`store_application_time` is `rglRound64((float)DXUtil_Timer(TIMER_GETAPPTIME) * 1000.0f)` — the
call `FUN_1400b97f0(5)` returns QPC seconds since the app-time baseline and the constant at
`0x140577c70` is `1000.0f`. `store_cur_mission_template_no` reads `[0x140620078]+0x9ae8`, or -1
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
`str_store_regex_replace` (4243), and `str_decode_url` (4244). Operands resolve through `0x140219df0`: tag 0 (`WseStringBanks::registers`)
is the writable 128-entry string register table at `0x140666060` (assigned by `0x140310b20` and
copied out by `0x140311730`), tag 0x16 the local string registers (`[0x1406227a8]`, count
`[0x1406227b4]`), and tag 3 or an unknown tag the global registers (`[0x1406227a0]`, count
`[0x1406227b0]`). `mbCheckStringRegister` (`0x1401c5370`) validates operand indices and logs on
failure (`WseDispatch::ignored`), while destination tag-0 registers use `mbCheckRegister` (`0..127`).
Comparisons use `wcscmp`/`_wcsicmp`; `str_compare` normalizes the case-sensitive path to `-1/0/1`
(`0x1401f8830`) and returns the raw folded code-unit difference from `_wcsicmp` (`0x140499764`) when
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
the 128-entry float array `DAT_140699748` (`g_basicGame+0x35d58`); `mbCheckRegister` (`0x1401c4fe0`) accepts indices 0–127.
`fld` divides the fixed-point operand by 100 (`g_basicGame+0x1f8` = `0x140663be8`, set to `0x64` at
`0x14007115a` in `mbBasicGame::initialize`); `fld_str` (`4401`) parses operand 1 with `atof`
(`0x14049975c`);
`fst` multiplies by 100 and rounds with `rglRound64` (`0x1402a1b80`): `floorf(x)` plus one when the
fraction is greater than 0.5. Float comparisons use `rglAbs` and `FLT_EPSILON`. `fpow`, `fmod`, and
`fatan2` take operand 1 in `xmm0` and operand 2 in `xmm1`; `flog` reads a literal base from operand 2
(default 10, minimum 2). `feval` (`4437`) runs `mbBasicGame::parseString` (`0x140072210`, turning literal
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
- Verified against the v1.1.5.6 control flow: `rglResourceManager::createMetaMesh` (`0x1402f3030`)
  requires `prefix.length >= 2` (`0x1402f31b3`), splits at the first `.`, and routes dotted non-LOD
  names to LOD `0`; `rglResourceManager::tryGetTexture` (`0x1402f35a0`) + `rglTexture::initDeviceObjects`
  (`0x1403184d0`) probe module language, module `Textures`, `TexturesHD` (single-frame only, before
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
- **SAF rename / move (`wse/platform/saf_vfs.h`, `platform/android/saf_vfs_jni.cpp`):** `SafTreeBackend::rename_document` and `move_document` default to unsupported. `SafVfs::rename_file` moves the document when its parent changes, renames it when its name changes, and deletes an existing target first, since SAF has no atomic replace. That delete is not undone when the move or rename then fails, so the target can be left missing (the source stays in place). A backend without support returns false and callers keep the remove + write fallback. The Android path (`WseSafBridge.renameDocument` / `moveDocument` over `DocumentsContract`, API 24) is compile-checked only.

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
  - **Rewriting:** loose uniforms → block members, except where a local, parameter, `for` variable
    (scoped to its loop body since the P8 review; it used to leak to the end of the function), or
    struct member shadows the name. Type names are matched exactly (`mat2`..`mat4x4`, `dmat*`,
    `[iu]sampler*` / `[iu]image*` dimensions), so uniforms such as `matWorld` are never taken for
    types; `isampler*` / `usampler*` / `image*` uniforms get a binding like `sampler*`; `#define` bodies get the same renaming (macro parameters
    excluded) when the source is not preprocessed. Top-level `struct` definitions are emitted
    before the uniform block. `gl_FragColor` → location-0 output; `gl_FragData[n]` → a location-0
    output array sized by the highest literal index (`4` for a computed index); writing both is
    an error. `texture{1D,2D,3D,Cube}{,Proj,Lod,ProjLod}` and the `*ARB` lod/grad forms →
    `texture*`; `shadow2D{,Proj,Lod,ProjLod}` → `_wse_shadow2D*` helpers returning `(r, r, r, 1)`
    (the GLSL 1.20 `vec4` result in `GL_LUMINANCE` depth mode). `invariant` is kept on vertex
    outputs only; `centroid` / `flat` / `smooth` / `noperspective` are kept.
  - **GLSL 1.30 interface:** file-scope `in` / `out` lower like `attribute` / `varying`; fragment
    `out` variables take locations `0, 1, ...` in declaration order (the GL default without
    `glBindFragDataLocation`); mixing them with `gl_FragColor` / `gl_FragData` is an error.
  - **Compatibility-profile built-ins (port choice):** `gl_Vertex` / `gl_Normal` / `gl_Color` /
    `gl_SecondaryColor` / `gl_MultiTexCoord0` alias attribute locations `0` / `1` / `3` / `4` / `2`
    (Warband's slots for the same data; `gl_MultiTexCoord1..7` and `gl_FogCoord` are dynamic);
    `gl_FrontColor` / `gl_FrontSecondaryColor` (read as `gl_Color` / `gl_SecondaryColor` in the
    fragment stage), `gl_TexCoord[]` (sized by the highest literal index in either stage, `8` for a
    computed one) and `gl_FogFragCoord` become shared varyings; `gl_ModelViewMatrix`,
    `gl_ProjectionMatrix`, `gl_ModelViewProjectionMatrix`, `gl_NormalMatrix`, `gl_TextureMatrix[]`
    and the inverse / transpose matrices become block members that keep their `gl_` name in
    reflection; `ftransform()` is `gl_ModelViewProjectionMatrix * gl_Vertex`. Fixed-function state
    structs (`gl_LightSource[]`, `gl_Fog`, `gl_FrontMaterial`) and `gl_BackColor` are not provided.
  - **Info log:** a successful compile returns glslang's parse warnings (shader log) followed by
    the link and SPIR-V builder logs.
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
    (`0x20`, `0x1402e8734`) when a shader is resolved (and uses the material's specular
    map/color/coeff rule when unshaded); `alpha_ref_u8` (`0x1402e8719`/`0x1402a0260`) stays
    active even on blended materials; `render_order` signed nibble (`-8..+7`) matches
    `rglMesh::updateRenderOrder` (`0x1402ba165`); blend `7` (`0x700`) is per-draw vertex alpha
    (`0x1402e93b9`); DX9 sets no sRGB texture/write states (`rglMesh::gammaCorrect` `0x1402c0520`
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
  sample (intentional improvement over retail `0x140333b40` `rand() % m_length`, which only reached
  the first 32767 frames).
- **Music scheduler:** `play_track` for the track already playing (option 1 or 2) keeps it playing
  and cancels a pending fade instead of stopping it and then dropping the request. `update()`
  re-bases its stored timestamps when the caller's clock jumps backwards. Clocks are `double`.
- **`last_sound_channel` / `615`:** the mixer reports the result of the most recent play request,
  `-1` included, matching what `599`/`600`/`1750`/`1881` store.
- **Opcodes:** `1881` stops the prop's previous channel before playing (one sound per prop
  instance); `1750` stops the agent's previous channel only when it is looping (one-shots finish)
  and passes `compute_agent_voice_pitch(agent_no, troop_no, party_no)` (`0x140057170` `m_no`,
  `m_troopNo`, `m_partyNo`; `0.3f` scale, `1.0f / (1.0f - delta)` for negative `delta`, player agent
  kept at `1.0f`) for human agents. `WorldAudioBridge::update` moves agent/prop channels to their
  owners' positions each tick. A sample that fails to read or decode is remembered and not retried
  on every play.
- **Not implemented / verified flags:** Doppler (`velocity`), HRTF (`FMOD_INIT_HRTF_LOWPASS` `0x10`
  at `0x140333d40`). `0x140333b40` has no random frequency jitter (`kSoundFlagDisableFreqVariation`
  `0x8` gates caller pitch multiplier and streaming); `distance_filter` gates 3D distance rolloff
  instead of FMOD's `FMOD_INIT_DISTANCE_FILTERING` (`0x200`) distance low-pass.
- **Headless audio (`wse/game/headless_runner.h`, `wse_headless --audio`, port infrastructure):** `HeadlessRunOptions::drive_audio` attaches a `WorldAudioBridge` before `game_start`, so sound and music opcodes reach the mixer. Each frame the listener follows the camera (`listener_from_camera_transform`, `mbSoundManager::setTransform` `0x140333730`), `WorldAudioBridge::update` runs, and `pump_to_latency` mixes one frame of audio into a `CaptureAudioDevice` that is never drained. `HeadlessReport::audio_stats` and `audio_frames_mixed` record the load and mix results. `wse_headless` links no codec, so only WAV samples decode unless a caller passes `audio_codecs` (app shells use `create_audio_codec_backend()`). `preload_audio` (`--preload-audio`) decodes every sample at load, removing the decode hitch inside a play opcode. The mixer thread and pause/resume are the audio worker below.
- **Audio worker (`wse/platform/audio_worker.h`, port infrastructure):** `AudioMixerWorker` owns the one mutex that guards a `WseAudioMixer` (which has no locking of its own). Its thread tops the device queue up to a target latency (`pump_to_latency`, default 2048 frames in 512-frame blocks, woken every 4 ms); game-thread mixer calls go through `lock()` / `with_mixer`, including `WorldAudioBridge::update` and anything that reads state written by channel-end callbacks, which now run on the worker thread. `set_paused` pauses the device and stops the top-ups. The SDL app keeps its audio stream open for its lifetime with a mixer on this worker, and its lifecycle event watch pauses it on `SDL_EVENT_WILL_ENTER_BACKGROUND` and resumes it on `SDL_EVENT_DID_ENTER_FOREGROUND`. Not done: the SDL app runs no game world, so no `WorldAudioBridge` or codec backend is attached there yet.
- **Settings (`wse/platform/performance.h`, port infrastructure):** `config/performance.json` is the one saved copy of the launcher and performance settings. `PerformanceSettings` gained `gyro_aim_enabled` / `pinch_zoom_enabled` (files without them load the defaults). `load_performance_settings_or_default` reads it (a missing or corrupt file gives the defaults), and `save_launcher_choices` writes the launcher's fields (cap, present mode, thermal / dynamic-resolution / LOD toggles, gyro, pinch) over the stored file, keeping `min_resolution_scale` / `max_lod_bias`. On Android the file lives under the app's `filesDir`: the launcher reads and writes it through JNI (`nativeLoadPerformanceSettings` / `nativeSavePerformanceSettings`, migrating the old `SharedPreferences` keys once) and `WseActivity` loads it into `RuntimePerformanceHub` (`nativeApplyPerformanceSettings`); the intent extras are gone. `RuntimeLaunchPerfConfig` now carries the whole `PerformanceSettings`, so the SDL loop applies present mode and the thermal toggles too. Desktop SDL builds read the file from `SDL_GetPrefPath`.

P7 input (`wse/platform/input.h`, `wse/game/world_input.h`, `platform/sdl/sdl_input.{h,cpp}`):

- **`WseGameKeyManager` & raw keys:** exact 125-entry `rglInputManager::getKeyName` (`0x1402ad5d0`)
  table and 95-slot `mbGameKeyManager` (`0x1406a0c00`, `0x1400e9800`) defaults (`75` built-in
  actions `MoveForward`..`TakeScreenshot` + `Custom1..20`, `Left Ctrl` modifiers, `13` gamepad
  alt bindings `0xf0..0xff`), primary+alt `keyDown`/`keyClicked`/`keyRaised` (`0x1400eef50`..
  `0x1400ef0b0`), `getKeyData` (`0x1400ef2f0`), and `3100` `game_key_get_key` (`0x1401e6bc2`:
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

Phase 5 R0 recompiler (`wse/recomp/`, `src/recomp/`):

- **Decoder subset (`x86_decode.h`, `x86_interp.h`):** `decode_insn` rejects anything outside its
  R0 leaf integer subset (legacy prefixes other than REX, `AH`..`BH` byte registers, `adc`/`sbb`,
  calls/indirect branches, SSE, truncated encodings) with an error naming the address and up to
  6 opcode bytes (no guessing). `decode_function` rejects control flow leaving the function range
  or branching into the middle of an instruction.
- **Plain guest memory accesses (`src/recomp/lifter_llvm.cpp`):** translated guest loads and
  stores are plain LLVM loads and stores in R0 (TSO-to-ARM64 memory-ordering lowering lands in R3
  per `docs/RECOMP_PLAN.md`).
- **R0 ELF loader (`src/recomp/r0_spikes.cpp`):** the anonymous-memory fallback loader
  (`LoadedCode::load_own`) accepts only 64-bit `ET_DYN` ELF objects with `PT_LOAD` segments,
  `.dynsym` symbols, `R_AARCH64_RELATIVE` / `R_X86_64_RELATIVE` relocations, and zero `DT_NEEDED`
  entries (external imports and all other relocation types are refused).
- **R1 runtime faults (`runtime.h`):** a dispatch to an untranslated VA, a callee returning to
  the wrong VA, an untranslated instruction, an import with no shim and an unset segment base
  all go to `Runtime::on_fault`; the default prints the fault with its address and aborts (no
  translation during play). After a fault `rip` is set to `Runtime::kFaultRip`, so the caller's
  return check stays quiet instead of reporting the same fault twice.
- **R1 `#DE` (`lift_r1_int.cpp`):** a zero divisor or a quotient that does not fit calls the
  `unsupported` hook with `#DE` in place of raising the exception (Win64 SEH arrives in R3).
- **R1 aligned `lock` (`lift_r1_int.cpp`):** `lock`-prefixed operands are lowered as naturally
  aligned LLVM atomics; a misaligned one is not atomic-safe on the phone (x86 would bus-lock).
- **R1 SSE (`lift_r1_sse.cpp`):** `cvtsd2ss`/`cvtpd2ps` round to nearest whatever MXCSR.RC says.
  Float-to-int conversions give the x86 "integer indefinite" value out of range instead of LLVM
  poison. NaN results follow Intel's rule bit for bit (`x86_nan`: first source, second source,
  then the negative real indefinite), and min/max quiet their first operand so LLVM's AArch64
  `fmaxnm`/`fminnm` lowering agrees with x86 on signaling NaNs.
- **R1 string instructions (`lift_r1_string.cpp`):** `gs:`/`fs:` overrides, `lock` and 16-bit
  addressing are reported unsupported; there is no `memmove`/`memset` fast path (it would import
  libc).
- **R1 traps and gated code (`lift_r1.cpp`):** `int3`, `int n`, `ud0`-`ud2` and `hlt` lift as a
  call to the `unsupported` hook (a fault with the address if reached); VEX/EVEX, SSE4.2 and
  `xgetbv` lift the same way, because `Runtime::hook_cpuid` does not advertise them.
- **R1 `rsqrt`/`rcp` (`lift_r1_sse.cpp`):** exact `1/sqrt(x)` and `1/x` instead of x86's
  vendor-specific 12-bit approximation (within its documented error bound).
- **R2 faults and timeouts (`r0_spikes.cpp`, `tools/r2_pc_record.cpp`):** a fault inside a
  differential case is a result, not a crash (a `sigsetjmp` guard on the phone, a vectored
  exception handler on the PC); a case running over 2 s stops the function (the PC skips it, the
  phone counts it as a mismatch). A function that writes the image's writable sections adds a
  marker to its hash and the sections are restored.
- **R3 exceptions (`seh.h`, `lift_r1.cpp`):** an exception nobody handles faults with the raise
  address ("unhandled guest exception") instead of terminating the process through
  `SetUnhandledExceptionFilter`; an unwind whose target frame is not on the guest stack faults and
  names the frames it walked. Nested exceptions, collided unwinds and rethrows from a catch block
  follow Windows (`seh.h` `Boundary`). A frame resumes only if its function has a `.pdata` handler (`ImageInfo::resumable`);
  unwinding into any other frame faults ("unwind into a frame with no exception handler").
  Catch continuations come from the `FuncInfo` try-block maps (`find_eh_continuations`); a
  continuation that a funclet computes some other way than `lea rax, [rip + X]` is not found and
  faults at the resume ("unwind continuation is not a block of this function").
- **R3 thread environment (`teb.h`):** a TLS, FLS or last-error shim called on a `CpuState` with no
  TEB (`teb::Process::attach` not called) faults (`unsupported`, naming the shim) instead of
  reading address 0. `TlsAlloc`/`FlsAlloc` with every slot taken return `TLS_OUT_OF_INDEXES` with
  `ERROR_NO_MORE_ITEMS`; `TlsFree`/`Fls*` on a free or out-of-range index return 0 with
  `ERROR_INVALID_PARAMETER`, and `TlsGetValue` out of range returns 0 with the same error.
  `TlsGetValue` clears the last error on success, as Windows does. One module's static TLS
  (`_tls_index` = 0); the translated DLLs' TLS directories are not read yet.
- **R3 threads and sync (`threads.h`, `kernel.h`):** `TerminateThread` of another thread is
  deferred: the thread exits at its next import call, wait or sleep, never in the middle of
  code (a thread blocked in `EnterCriticalSection` is not woken by it). `EnterCriticalSection`
  on a section that was never initialized and `LeaveCriticalSection` by a thread that does not
  own it fault (`unsupported`) instead of corrupting the lock; `SleepConditionVariableCS` without
  owning the section faults. `SetThreadAffinityMask` and `SetThreadPriority` are recorded and
  returned, not applied to the host thread; `GetThreadTimes` reports zeros. Every wait wakes on
  any signal and re-checks (one host condition variable). `Threads` joins its host threads when
  destroyed (and joins finished ones whenever a thread starts).
- **R4 time (`shim_time.h`):** the performance counter runs at 10 MHz (Windows' frequency depends
  on the machine; programs read it); the local time zone is a fixed offset from the host (or
  the embedder) with no daylight-saving rules, and `GetTimeZoneInformation` reports
  `TIME_ZONE_ID_UNKNOWN` with that bias.
- **R4 CRT support and system information (`shim_crt.h`):** `GetVersionExW` reports 6.2 build
  9200, what Windows reports to an exe that declares no supported OS. `IsProcessorFeaturePresent`
  answers from the fixed CPU profile (SSE to SSE4.1 yes; SSE4.2, AVX and later no), whatever the
  host CPU has. `ExitProcess` reports the code to the embedder and unwinds the calling thread;
  the embedder stops the others. `GetCurrentProcessorNumber` is always 0. Timer-queue timers
  (`threadpool.h`) get one worker each, like thread-pool timers.
- **R4 strings and locale (`shim_strings.h`):** en-US with ANSI code page 1252 and OEM code page
  437, Windows' English defaults, whatever the host is set to (Windows itself can run with 65001
  everywhere; the user's PC does). The 1252 and 437 tables, case mapping and `CT_CTYPE1` for
  Latin-1, Latin Extended-A, Greek, Cyrillic and General Punctuation, UTF-8 in both directions
  (including Windows' replacement of invalid sequences) and the word-sort order of ASCII text
  were checked against the Windows APIs on the user's PC; other scripts use simple ranges. No
  best-fit mapping: a character the code page lacks becomes the default character. Sort keys
  are the code units. `GetLocaleInfo*` answers a fixed en-US table; an unknown `LCTYPE` fails
  with `ERROR_INVALID_FLAGS`. `FormatMessage` knows a short table of system messages and does
  not read message tables from modules.
- **R4 files (`shim_files.h`):** guest paths map to three mounts: `C:\WSE2` (the install, read
  through an overlay whose changes go to the writable layer under `install/`; deleting an install
  file only hides it, for the session), `C:\Users\Player` and `C:\ProgramData` (the writable layer);
  anything else is `ERROR_PATH_NOT_FOUND`, and the mounts' ancestors (`C:\`, `C:\Users`) are
  read-only directories. An open file holds the file's bytes, read at open and written back on
  flush or when its last handle closes, so two handles on one file do not see each other's
  unflushed writes. Directory handles and overlapped I/O are not supported (`ERROR_ACCESS_DENIED`).
  Every file reports the same time (2020-01-01); attributes are `DIRECTORY` or `ARCHIVE`. Locks
  always succeed (one process). File mappings copy the file; a writable view goes back to the file
  on `FlushViewOfFile`/`UnmapViewOfFile`, and views are 64 KiB-aligned. Like the GUI exe the
  process has no console: the standard handles are null unless the embedder asks for one.
- **R4 memory (`shim_memory.h`):** heap blocks carry a 16-byte header; freeing or sizing a pointer
  no heap here allocated fails instead of corrupting memory. `HeapDestroy` does not reclaim the
  heap's blocks, and `HeapWalk` enumerates nothing. Local/Global memory is fixed (the handle is
  the pointer). Virtual memory is real host address space, always read-write: protections are
  recorded for `VirtualQuery`/`VirtualProtect` but not enforced, and a request for a fixed address
  outside a region reserved earlier fails with `ERROR_INVALID_ADDRESS`.
- **R4 modules and resources (`shim_modules.h`, `pe_image.h`):** modules never unload:
  `FreeLibrary` only counts, and no `DLL_PROCESS_DETACH`, `DLL_THREAD_ATTACH` or
  `DLL_THREAD_DETACH` call is made (Windows sends the thread notifications to every DLL that did not
  call `DisableThreadLibraryCalls`). Names compare by file name only: a path's directory is
  ignored, so `C:\elsewhere\lua5164.dll` finds the loaded one. `LoadLibraryEx` with a data-file
  flag returns the ordinary handle. A system DLL (KERNEL32, KERNELBASE, ntdll, each DLL an image
  imports, and any DLL `LoadLibrary` names that has shims) is a 64 KiB fake image holding only
  headers; `GetProcAddress` on it succeeds only for a function that has a shim (the CRT's optional
  lookups then take their fallbacks), by ordinal only for one registered as `"#n"`; the address it returns runs the shim
  when called and is not code. API-set names resolve to KERNEL32. `FindResource` picks the neutral
  language, then en-US, then the first present, which is what Windows picks for an en-US user
  (it looks at the thread's UI languages first); `FindResourceEx` with a language not present
  fails with `ERROR_RESOURCE_LANG_NOT_FOUND` (checked on Windows), and a module without resources
  with `ERROR_RESOURCE_DATA_NOT_FOUND`. Base relocations of type ABSOLUTE, HIGHLOW and DIR64 are
  applied; any other type fails the load.
- **R4 ADVAPI32 (`shim_advapi.h`):** the registry is one tree in memory, written to `NTUSER.DAT` at
  the root of the writable layer (our own text format; HKLM is kept there too) after every change.
  Only the four predefined roots exist until something creates keys below them; there are no
  security descriptors, and `samDesired` is ignored. `Crypt*` hashing supports MD5, SHA-1 and
  SHA-256 (checked against the RFC 1321 / FIPS 180 vectors); any other algorithm fails with
  `NTE_BAD_ALGID`, and a hash takes no more data once its value has been read.
- **R4 USER32 (`shim_user32.h`):** headless. A window has no non-client area: its client size is its
  size and `AdjustWindowRect` leaves the rectangle as it is. Nothing is painted (`UpdateWindow`
  sends no `WM_PAINT`), `TranslateMessage` produces no `WM_CHAR` (no keyboard layout yet), and
  `MessageBoxA` reports to the embedder and answers `IDOK`. Message queues are per host thread;
  `GetMessageW` blocks on it, `PeekMessageW` filters by window and range, `WM_QUIT` passes every
  filter. The clipboard holds text formats (the data is copied up to its terminator). IMM32 has
  no input method (`ImmGetContext` returns NULL); GDI stock objects are fixed handles.
- **R4 offline network (`shim_net.h`):** `WSAStartup` succeeds (version 2.2), but `socket` and
  `WSASocketW` fail with `WSAENETDOWN`, every socket call with `WSAENOTSOCK`, and name lookups with
  `WSAHOST_NOT_FOUND`; `gethostname` is `wse2-android`. `inet_pton`/`inet_ntop` handle IPv4 only
  (`WSAEAFNOSUPPORT` otherwise). WinHTTP hands out handles, but sending a request fails with
  `ERROR_WINHTTP_NAME_NOT_RESOLVED`; `URLDownloadToFileA` fails with `INET_E_RESOURCE_NOT_FOUND`.
  WS2_32's imports by ordinal bind through its fixed export ordinals.
- **R4 small system DLLs (`shim_system.h`):** symbol and stack-walk functions fail with
  `ERROR_NOT_SUPPORTED` (`SymInitialize` succeeds, so a crash handler degrades instead of
  stopping); `MiniDumpWriteDump` writes nothing. COM initializes but `CoCreateInstance` answers
  `REGDB_E_CLASSNOTREG`. `SysFreeString` of a BSTR it did not allocate is ignored. `UrlEscapeA`
  escapes what is not RFC 3986 unreserved or a URL delimiter, a rule, not Windows' table.
  `ShellExecuteA` and `CreateProcessA` start nothing (reported to the embedder). XInput has no
  controller. `GetProcessMemoryInfo` reports zeros.
- **R4 D3DX math (`shim_d3dx.h`):** compared with the system's `d3dx9_43.dll` on the user's PC over
  20,000 random cases per function: all agree to 1e-4 relative, except the inverse of
  ill-conditioned matrices, which both compute to float accuracy (the same residual
  `|M * inverse - I|`) with different rounding. A singular matrix returns NULL and leaves
  `*pDeterminant` as it was, as D3DX does.
- **R4 recording D3D9 device (`shim_d3d9.h`, `com_vtables.h`):** nothing is drawn. One adapter with
  eight display modes (640x480 to 1920x1080, X8R8G8B8 only, 60 Hz), Direct3D 9 caps of a shader
  model 3.0 device (`D3DCAPS9` filled for the fields the comments name, zeros elsewhere), every
  format and depth match reported supported, multisampling only off. Resources have real memory,
  allocated when first locked; render, sampler and texture-stage states, transforms, viewport,
  scissor, bound textures, shaders, buffers and declarations are stored and returned. Queries
  finish at once (an event query reads TRUE). Objects are never freed (Release only counts).
  D3DX textures take size, format and levels from a DDS header when the file can be read, else
  256 x 256 ARGB; a missing file is `D3DXERR_INVALIDDATA`. Effects hand out opaque handles and run
  one pass; `D3DXSaveSurfaceToFileA` writes nothing. DirectInput 8 devices report nothing pressed
  and no buffered events; `EnumDevices` lists nothing. A method outside these and the "recorded"
  names (`Set*`, `Begin*`, `End*`, `Draw*`, `Clear`, `Present`, ...) faults with its name.
- **R4 code-relative offset tables (`discover.cpp` `find_offset_table_entries`):** a table is
  recognised only from code that loads a 16-bit value from it and adds a register holding a
  `.pdata` function's start within 8 instructions (tracked in address order through each
  function, without data flow across branches); it is read from the lowest address seen until a
  value falls outside that function, so a table followed by small numbers would contribute a few
  wrong entries (functions that are never called). Not used by the R1 coverage meter or R2.
- **R4 tail calls (`lift_r1.cpp`):** a tail jump to a function of the same module is `musttail`; a
  tail jump through the dispatch or import hook is marked `tail`, which holds at -O1 and up, and
  `Runtime::hook_dispatch` itself ends in a sibling call only when the runtime is optimized
  (every gate builds `RelWithDebInfo`). A `Debug` runtime therefore grows the host stack on every
  indirect tail jump: an interpreter loop (LuaJIT's) would overflow it.
- **R4 SEH additions (`seh.h`):** `RtlUnwind` is `RtlUnwindEx` without a context record;
  `RtlRestoreContext` resumes at the context through `unwind_rsp` like an unwind, so the target
  must be a block start of a function with a `.pdata` handler. In such a function every call's
  return address is a block start (`lift_r1.cpp`), so an MSVC `longjmp` back to a `setjmp` call
  site, or a restore of an `RtlCaptureContext` context, resumes there; a function with no handler
  still cannot be resumed; `STATUS_LONGJUMP` takes rbx, rsp, rbp, rsi, rdi, r12-r15, rip, MXCSR and
  xmm6-xmm15 from the `_JUMP_BUFFER` (FpCsr is ignored: no x87 state). Each translated image's
  `.pdata` is used for the frames inside it (`Exceptions::add_image`).
- **R3 thread pool (`threadpool.h`):** one worker guest thread per timer or wait object, so one
  object's callbacks never overlap (Windows may run a periodic timer's callbacks concurrently when
  one runs long); a periodic timer's next due time is the previous one plus the period.
  `UnregisterWaitEx` with an event waits for running callbacks and signals the event before it
  returns (Windows returns at once and signals later). `WaitForThreadpool*Callbacks` and
  `UnregisterWaitEx` called from the object's own callback do not wait for themselves.
- **R3 memory ordering (`lift_r1.cpp` `Ctx::load`/`store`):** shared accesses are acquire/release
  when aligned and barrier-fenced when not; `rsp`-based operands are treated as thread-local stack.
  A stack address passed to another thread would be accessed without ordering (accepted for now).
- **Import-free modules (`lifter_llvm.cpp`):** translated modules link with `-z defs`; on an
  x86-64 host the target CPU is `x86-64-v2` so `llvm.floor`/`roundeven` stay inline.
- **R0 (d) input bounds (`r0_inputs.h`):** `make_r0_index_of_cases` keeps `start` in `[0, len]`
  because retail `rglString::indexOf` (`0x1403103f0`) does not check `start`. `pe_image.h`
  bounds-checks all PE32+ headers, sections, and `.pdata` reads against the input buffer.

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
    never created; `mbCheckPath` (`0x1401c57c0`) rejects
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
placeholders until each is checked against the v1.1.5.6 disassembly:

- **Simplified helpers in `world_campaign_helpers.h` (no VA evidence yet):** `party_add_template`
  adds the midpoint of each stack's min/max instead of a random count and ignores
  `set_party_creation_random_limits` (1080); `troop_add_merchandise` fills free slots round-robin
  and ignores faction, abundance weighting, and item probability; `troop_equip_items`,
  `troop_sort_inventory`, `troop_ensure_inventory_space`, and the XP/upgrade helpers use simple
  local rules.
- **Simplified combat:** `agent_deliver_damage_to_agent` (1722) applies weapon damage times the
  deliverer's damage modifier with no armor, skill, or hit-location model.
- **Checked against v1.1.5.6 and correct as written:** `store_num_free_stacks` (2154,
  `0x1401dd77a`) stores the constant `10` in retail too; `troop_is_mounted` (152, `0x1401c704b`)
  tests equipment slot 8 (`mbTroopEquipment[8].isValid`), not a troop flag; `entering_town` (36,
  `0x1401c65c8`) needs main-party behaviour 4 toward the town, a readable town, and a distance
  *greater* than `2.3 × main-party radius`; `get_sq_distance_between_positions` (712,
  `0x1401caf6a`) is `lengthSquared × 100`, truncated.
- **Party limits and skill levels (fixed):** `mbParty::getMaxNumCompanions` (`0x1401c18a0`) and
  `getMaxNumPrisoners` (`0x1401c1920`) run the module's `game_get_party_companion_limit` /
  `game_get_party_prisoner_limit` (mapped slots `0x15` / `0x16`, params party and 0) and use its
  trigger result; only without the script do they use `20 + leadership × 3 + charisma` and
  `prisoner_management × 5` (`rglConfig::Campaign` `iBaseCompanionLimit` / `iLeadershipSkillBonus`
  / `iPrisonerManagementSkillBonus`), with the leader's (stack 0) skills from
  `mbGame::getTroopSkill(..., true)` (`0x1400dcd50`), which adds the module's
  `game_get_skill_modifier_for_troop` (slot `0x1b`, trigger result reset first) and clamps to
  `[0, max level]`. The port used `30 + charisma + 5 × party leadership` and ignored all three
  scripts. `vm::party_max_companions` / `party_max_prisoners` / `troop_skill_level`
  (`block_vm.h`) now back `hero_can_join` (101), `hero_can_join_as_prisoner` (102),
  `party_can_join` (103), `party_can_join_as_prisoner` (104), `troops_can_join` (105),
  `troops_can_join_as_prisoner` (106), `party_can_join_party` (107),
  `party_get_free_companions_capacity` (1633), `party_get_free_prisoners_capacity` (1634) and
  `store_skill_level` (2170, `0x1401dda60`).
- **Scripts run from inside a handler get their own frame (fixed):** retail
  `mbStatementBlock::execute` (`0x1401f6b50`) takes its locals and loop stack from a per-call pool,
  so a mapped script a handler starts (`store_skill_level`, the party limits, opcodes that run
  mapped slots `13`, `19`, `0x27`) never touches its caller's. The port ran every
  `execute_script` at depth 0, so a script called from a depth-0 block overwrote that block's
  locals and loop frames; outer `try_for_range` loops restarted, which is what stalled Phantasy
  Calradia when 2170 first ran the modifier (it looked like a slow interpreter; a microbenchmark
  measures 35–61 ns per statement). `VmContext::activeBlocks` now sends a nested call to
  `currentDepth + 1` and keeps the caller's `this_or_next` state, and the frame vectors reserve
  every allowed depth on first growth so a handler's `locals` span cannot dangle after a nested
  call. **Safety deviation:** a party with no stacks has no leader, so the
  formula uses skill 0 and charisma 0 (retail reads stack 0 unchecked).

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
scene tests: PASS
ui tests: PASS
recomp tests: PASS
selftest_host tests: PASS (24/24, ...)
engine host tests: PASS
```

To run the headless Phase 2H validator against a local Warband install:

```powershell
.\engine\build\host\wse_headless.exe "C:\Path\To\Warband" "Modules/Native" --frames 20 --step-ticks 2500000
```

The Android NDK (`aarch64-linux-android29`) header checks, cross-compile build, and `arm64-v8a` SDL3 debug APK (`gradle -p android assembleDebug` -> `libwse2.so` + `libSDL3.so`) also run in `.github/workflows/ci.yml`; see [`STATUS.md`](../STATUS.md) §2 for current gate results.
