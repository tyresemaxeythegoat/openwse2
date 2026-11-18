# STATUS

Last updated: 2026-11-18.
Phase: **2 — Core Data Structures & Module System VM** (Phase 1 triage is complete; see `ROADMAP.md`).

This file is the **only** place that holds live numbers (gate results, coverage counts). Other
documents link here instead of repeating them — see the ownership table in `AGENTS.md` §4.

## 1. At a glance

| Area | State |
| --- | --- |
| Evidence baseline | WSE2 **v1.1.5.5** x64 (PDB + PE + Ghidra 12.1.4). The 2026-10-14 build is superseded. |
| Host gate | **PASS** on Windows (2026-11-18, `engine/build_host.ps1`, Zig `clang++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror`: `wse_lua` + `wse_audio` + `wse_glslang`/`wse_shader` + `driver_loader` + `24/24` standalone suites / `219` `test_*` functions + `selftest_host` `24/24` `10,437 ms` + `wse_headless` + `wse_brf`, `engine host tests: PASS`; prior `WSE_WARBAND_ROOT` retail run verified `Native` `221/221` & `Phantasy Calradia` `333/333`, `271/271` shaders at `spirv_fail=0`, `wse_headless` `20` frames / `755` parties `errorCount=0` `unimplementedCount=0`). `Engine CI` passed `4/4` on `ca0163a` (PR #21) and re-runs on push; `recomp-host` and `llvm-android` added, first run pending |
| Android arm64 gate | **PASS on pre-R0 baseline `ca0163a` (re-runs on push; `llvm-android` added, first run pending)** `.github/workflows/ci.yml` (`android-arm64` + `android-apk`) compiles all headers standalone (`94` headers under `engine/include/`), cross-compiles CMake targets (`wse_engine`, `lua51`, `wse_lua`, `wse_vulkan`, `wse_audio`, `wse_glslang`, `wse_shader`, `wse_selftest`), and builds `arm64-v8a` debug APK (`libwse2.so` + `libpanvk_hw.so` + `libSDL3.so` + `libwse_r0.so` + `assets/scripts.txt` with `WseLauncherActivity` + `WseSafBridge` + `WseActivity`; `android-apk` now needs `llvm-android`); on-device runtime tests remain unrun |
| WSE extended opcodes | 440/440 mapped, **440/440 implemented** |
| Native (non-WSE) opcodes | **1,053 live handlers mapped** (`16` control-flow `0–29` + `1,037` switch `30–2767`, plus `6` `noop_true` and `1` SDK-named fallback `1004`; `3` live handlers `53, 2180, 2749` have no SDK constant and carry community names); **1,053/1,053 implemented (100%, 20/20 categories)** (`16` control-flow incl. 4 live entity iterators + `1,037` switch across `block_vm.h`, `native_ops.h`, `native_entity_ops.h`, `native_mission_ops.h`, `native_campaign_ops.h`, `native_campaign_ops_2.h`, `native_world_ops.h`, `native_world_ops_2.h`, `native_world_ops_3.h`, `native_agent_ops.h`, `native_mp_ops.h`, `native_scene_ops.h`, and `native_ui_ops.h`) |
| Module & scene loaders | Clean-room module & scene loaders implemented across `wse/game/{module_loader_common,module_world_loaders,module_asset_loaders,module_mission_loaders,module_bundle,scene_loaders}.h` reading exclusively via `wse::platform::Vfs` + `wse::io::{TokenReader,BinaryReader}` (`skills`, `factions`, `troops` v1/v2, `item_kinds` v1/v3, `party_templates`, `parties`, `scenes`, `quests`, `info_pages`, `strings`, `quick_strings`, `game_variables`, `module.ini`, `module_config.ini`, `meshes`, `actions`, `skins`, `sounds` v2/v3, `music`, `particle_systems`, `postfx`, `map_icons`, `scene_props`, `menus`, `presentations`, `mission_templates`, `tableau_materials`, `conversation` v1/v2, `scripts`, `triggers`, `simple_triggers`, `map.txt`, `SceneObj/*.sco`, `Data/skeleton_bodies.xml`, `Data/{item_modifiers,flora_kinds,ground_specs,skyboxes}.txt`) |
| World state & headless run (Phase 2H) | `WseWorldState` populated from module files via `wse::game::load_module` (`module_bundle.h`), binding `ScriptManager`, triggers, and owned UTF-16 `WseStringBanks` into `VmContext` / `WseVmState`; **Phase 2H headless runner** (`wse/game/headless_runner.h` + `engine/tools/wse_headless.cpp`) validated locally on user retail `Modules/Native` (`34` loaded files, `1` skipped optional `module_config.ini`, `game_start` + `20` trigger frames across `25` triggers and `110` simple triggers -> `755` parties, `0` VM errors, `0` unimplemented opcodes with `failOnUnimplemented = true`) |
| Lua 5.1 layer (Phase 2F) | **Complete (`lua51` + `wse_lua`)** — stock MIT Lua 5.1.5 (`2640fc56…`) wired behind `vm::WseLuaRuntime` (`wse/vm/lua_runtime.h`, `wse/vm/lua_manager.h`, `engine/src/lua/`) with `<lua.h>` isolated from headers, sandboxed `Vfs`/`Clock`/`LogSink` routing (`%storage%`, `%savegames%`, bytecode `\x1b` rejection), `bit` + clean-room `LuaGlobals`, `msfiles/` + `variables.txt` parsers, 36 `game.*` + 7 `_G.*` callbacks, trigger/presentation/psys registration, operation/script hooks, all 9 engine `On*` events, opcodes `5100`–`5114`, and `wse_headless` integration |
| Platform track (Phase 3/4) | Planned as slices P0–P9 in `docs/PLATFORM_PLAN.md`. **P0, P3, and P5 host path complete; P1, P2, P4, P6, P7, P8, and P9 Part 1 built and host-verified, on-device run pending hardware**: `wse/platform/{vfs,saf_vfs,clock,sleep,log,rhi,driver_package,rhi_brf,rhi_scene,audio,input,performance}.h`, `platform/vulkan/driver_loader.{h,cpp}`, and `platform/sdl/{sdl_audio,sdl_input}.{h,cpp}` implement the VFS (`MemoryVfs`, Windows/POSIX case-insensitive `HostVfs`, `SafVfs`), clocks/sleep/logging, minimal RHI (`RhiDevice`, `SoftwareRhiDevice`, `PresentationPath` selection, D3D9 RH projection/half-pixel/sRGB helpers, `ShaderModuleDesc`/`VkShaderModule` lifecycle, Blinn-Phong + normal/specular/alpha-test/5 blend modes, plus **P9 Part 1** chunked `WorldMapManifold` terrain & multi-draw/skinned `BrfSceneRenderer`), AdrenoTools/PanVK driver package importer + ELF `.dynstr` `libpanvk_hw.so` patcher, `VulkanDriverLoader` (`load_system`, direct-ICD v5 / custom driver preload, `MockVulkanIcd` v5, `VulkanRhiDevice`), stereo 2D/3D audio mixer + music scheduler (`WseAudioMixer` + `NullAudioDevice`/`CaptureAudioDevice` + `SDL_OpenAudioDeviceStream`), `WseGameKeyManager` / `WseInputManager` (`InputContext`, virtual stick/look/4-dir attack & block swipes/command radial, opt-in low-pass filtered `Mission` gyro aiming, `CampaignMap` two-finger pinch-to-zoom, `TouchLayoutEditor`, per-module `Vfs` JSON profiles, `FreeCameraController`, SDL3 event + `SDL_SENSOR_GYRO` backend), P8 performance & frame pacing (`FrameRateCap` 30/60/90/120 Hz + uncapped, `PresentModePreference`, hysteretic `ThermalPerformanceController` -> resolution scale / LOD bias / thermal FPS step-down, drift-free `FramePacer`, `RuntimePerformanceHub`, persisted `Vfs` JSON settings), and `wse/game/{audio_runtime,world_audio,world_input}.h` (`WorldAudioBridge` for sound opcodes + `WorldInputBridge` for `65`, `70..73`, `75`, `77..78`, `3100..3102` + `map_zoom_delta`); `platform/android/saf_vfs_jni.cpp` + Kotlin `WseSafBridge` + `WseLauncherActivity` + `WseActivity` implement `ACTION_OPEN_DOCUMENT_TREE`, `SafVfs`, `Modules/*/module.ini` scanner, GPU driver selector, P8 launcher settings toggles, Android `PowerManager` thermal listener/`getThermalHeadroom`, `ANativeWindow_setFrameRate*` + SDL3 pacing in `sdl_selftest_main.cpp`, `scripts.txt` verifier, `import_to`, and the `"Probe Vulkan Driver & Render Test Mesh"` probe; `wse/asset/{brf,dds,shader,shader_compiler,skeleton,audio,audio_codecs}.h` + `src/audio/` (`wse_audio` with `stb_vorbis` v1.24 + `dr_mp3` v0.7.5) + `src/shader/` (`wse_shader` with Khronos `glslang 16.6.0`) + `wse_brf` decode all 8 `.brf` resource types, skeletal bind-pose FK & CPU linear-blend skinning, `.dds` textures (incl. `BC1`–`BC5` and `L16`), `module.ini` catalogs, effect search order, `wse2_shaders.{brf,ini}`, material render state, `GLShaders` XML/GLSL + 5 WSE2 skinning variants, Warband GLSL → Vulkan `#version 450` SPIR-V 1.0 compilation, persistent `SpirvShaderCache` (`kWseSpirvGeneratorVersion = 4`), and WAV/OGG/MP3 audio streams/samples; `wse_selftest` aggregates all `24` core suites (`selftest_host_test` PASS on host); `platform/sdl/sdl_selftest_main.cpp` + `android/` Gradle (CMake targets `wse2`, `panvk_hw` + SDL3 `3.4.20` Prefab AAR) build the `arm64-v8a` APK. Next slice: `P9` remaining UI/Bullet convergence & on-device testing |

## 2. Gates

| Gate | Result | Detail |
| --- | --- | --- |
| Host C++20, Windows | PASS (2026-11-18) | `engine/build_host.ps1`, Zig `clang++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror`: `liblua51_cxx.a` + `wse_lua` + `libwse_audio.a` (`stb_vorbis` + `dr_mp3`) + `libwse_glslang.a` + `libwse_shader.a` (`glslang 16.6.0`) + `driver_loader` + `24/24` standalone suites / `219` `test_*` functions (`operand`, `trigger`, `savegame`, `script`, `wse_bitwise_fp`, `wse_system`, `wse_string`, `wse_dict`, `wse_array`, `wse_world`, `block_vm`, `native_world`, `native_m5`, `wse_m6`, `platform`, `module_loaders`, `headless`, `lua`, `asset`, `audio`, `input`, `scene`, `ui`, `recomp`) + `selftest_host_test.exe` (`24/24`, `10,437 ms`) + `wse_headless.exe` + `wse_brf.exe` (`engine host tests: PASS`) |
| Host C++20, Linux | PASS (`host-linux`, `host-sanitizers` and `recomp-host` green on `bf08818`, run `44331809617`; re-runs on push with `spirv-val`) | `.github/workflows/ci.yml` (`ubuntu-24.04`, clang++, CMake + CTest `-DWSE_BUILD_HEADER_CHECKS=ON`): standalone header checks (`94` headers), CTest (`24` suites + `selftest_host`, each suite compiled once into `wse_selftest`), `spirv-val --target-env vulkan1.0`, `extract_wse_opcodes.py --check`, and `check_citations.py --self-test --docs`; `recomp-host` (`clang++-18`, `-DWSE_ENABLE_LLVM=ON` against `llvm-18-dev` + `liblld-18-dev`) builds and runs `recomp_test` and `recomp_llvm_test` (synthetic code only) |
| Android NDK `aarch64-linux-android29` | PASS on pre-R0 baseline (`ca0163a`; re-runs on push; `llvm-android` added, first run pending) | `.github/workflows/ci.yml` (`ubuntu-24.04`, NDK Clang, `-std=c++20 -O2 -Wall -Wextra -Werror`): all `94` headers compile standalone + CMake cross-compile build; `llvm-android` builds and caches static LLVM + LLD `18.1.8` (`AArch64` target only) once; `android-apk` (`needs: llvm-android`, `gradle -p android assembleDebug` with pinned NDK / CMake -> `libwse2.so`, `libSDL3.so`, `libpanvk_hw.so`, `libwse_r0.so`, and `assets/scripts.txt` as the only asset) builds and uploads the APK artifact |
| Host PanVK loader/surface test (legacy) | PASS (recorded) | `host loader tests: PASS`; independent of the reconstructed engine |
| Host ASan + UBSan | PASS (CI run green on `e8b3594`; re-runs on push) | `.github/workflows/ci.yml` job `host-sanitizers` (`ubuntu-24.04`, clang++, `cmake -DWSE_SANITIZE=ON` + CTest). First-party code is fully instrumented; the unmodified third-party `lua51` target is built with `-fno-sanitize=float-cast-overflow` only (stock `lua_number2int` in `ltable.c`). PR #9 was merged with this job failing on `6f83abc` / `7be8d6d` / `fea4229`; `b6bb82b` restored it |
| CI | PASS on `413e95e` (all six jobs, run `44336216717`; re-runs on push) | `.github/workflows/ci.yml` (`Engine CI`): runs `host-linux` (incl. `spirv-val --target-env vulkan1.0`, `extract_wse_opcodes.py --check`, and `check_citations.py --self-test --docs`), `host-sanitizers`, `recomp-host`, `llvm-android`, `android-arm64`, and `android-apk` on push and PR |

## 3. Coverage

### WSE extended opcodes (440/440)

Counted automatically by `tools/extract_wse_opcodes.py --sync-status` from distinct
`case WseOpcode::…` labels under `engine/include/wse/vm/` against `docs/wse_opcodes.json`.
Per-opcode handler notes live in `engine/README.md`; this generated table is the sole tally.

| Block | Family | Done | Remaining examples |
| ---: | --- | ---: | --- |
| 2800 | shifts / xor / not | 10/10 | — |
| 2900 | player | 10/10 | — |
| 3000 | registers, script context, slots, timers, misc | 46/46 | — |
| 3100 | input keys | 3/3 | — |
| 3200 | dictionaries | 23/23 | — |
| 3300 | agent | 61/61 | — |
| 3400 | multiplayer messaging | 17/17 | — |
| 3500 | server admin | 21/21 | — |
| 3600 | mission / props / particles | 26/26 | — |
| 3700 | troop | 10/10 | — |
| 3800 | item / cur_item | 14/14 | — |
| 3900 | party | 12/12 | — |
| 4100 | position vectors | 5/5 | — |
| 4200 | strings | 47/47 | — |
| 4300 | options | 6/6 | — |
| 4400 | floating point | 39/39 | — |
| 4500 | scene | 23/23 | — |
| 4600 | edit mode | 10/10 | — |
| 4700 | reload / material | 4/4 | — |
| 4800 | menus | 4/4 | — |
| 4900 | overlays / presentations | 8/8 | — |
| 5000 | dynamic arrays | 23/23 | — |
| 5100 | Lua bridge | 15/15 | — |
| 5200 | skins / skeletons | 3/3 | — |

All 440/440 WSE extended opcodes across all 24 blocks (`2800`–`5200`) are implemented across `execute_wse.h` and `wse/vm/ops/*.h`.

### Native opcodes (`block_vm.h`, `native_*.h`, `docs/native_opcodes.json`)

Generated from `mbStatement::execute` jump table `0x1401e22f0` (`2,738` slots), `mbStatementBlock` control-flow dispatch (`0–29`), public `header_operations.py` (`1,057` unique IDs in `0–2767`), and live `block_vm.h` + `native_*.h` scan via `tools/extract_native_opcodes.py` (`docs/NATIVE_OPCODES.md`):

- Community cross-check (2026-11-14, <https://mbcommands.fandom.com/wiki/Operations>, `1,065` numbered operations): all `1,059` names shared with the map carry the same opcode number (`0` disagreements); the remaining `6` are the `2` modifiers, the `3` community-only names above, and `try_for_attached_parties` `15`, which has no handler in WSE2. The page does not list WSE2's `break_loop` 8, `continue_loop` 9, or `try_for_dict_keys` 18. Its `[cf]`/`[lhs]` tags differ from the SDK `can_fail_operations`/`lhs_operations` lists on `104` operations; the map keeps the SDK lists.
- Control flow `0–29` (**16/16 live** implemented): `call_script` 1, `try_end`/`try_begin`/`else_try` 3–5, `try_for_range` 6, `try_for_range_backwards` 7, `break_loop` 8, `continue_loop` 9, `try_for_parties` `0xB` (11), `try_for_agents` `0xC` (12), `try_for_prop_instances` `0x10` (16), `try_for_players` `0x11` (17), `try_for_dict_keys` `0x12` (18), `store_script_param*` 21–23. (`0xB` / `0xC` swapped naming in `block_vm.h` was fixed to match `startLoop` `0x1401f7516`/`0x1401f732e` and `header_operations.py`.) All four entity iterators (`11, 12, 16, 17`) are now live against `WseWorldState` (`parties`, `agents`, `mission_objects`, `players`) and preserve `ctx.currentCaller` across recursive calls (`0` entity-iterator stubs).
- Switch table `30–2767` (`2,738` slots):
  - **1,037** live unique `switch_handler` `.text` targets (incl. **3** live handlers with no `header_operations.py` constant, now named from the community operations reference in `docs/NATIVE_OPCODES.md` and implemented in `native_world_ops.h`: `53` `is_cheat_mode_enabled` reading `rglConfig::General::bCheatMode`, `2180` `store_main_party_wounded` counting wounded main-party regulars and wounded heroes while skipping prisoners per `0x1401ddb29`, `2749` `str_store_agent_face_keys`), all **1,037** of which (**100%**) are implemented across `native_ops.h` (**145**: `33` core in `dispatch_native_statement` — comparisons `30–33`, shifts/arithmetic/math/assign `2100–2101`, `2105–2117`, `2119–2131`, `2133` — plus `112` Milestone 1 extended handlers in `dispatch_native_statement_ext` across `entity_slots`, `positions_and_math`, `arithmetic_registers_and_vars`, `strings`, `troops`, `parties`, `agents`, and `ui_presentations_and_menus`), `native_entity_ops.h` (**108** Milestone 2 handlers across player/MP, quests, audio/UI/presentations/overlays, sites/factions, troops, and parties/campaign), `native_mission_ops.h` (**66** Milestone 2 handlers across agents, teams/mission combat, scene props, and mission timers), `native_campaign_ops.h` (**156** Milestone 3 handlers `36–1694`), `native_campaign_ops_2.h` (**128** Milestone 3 handlers `1695–2751`; **284** Milestone 3 native opcodes total), `native_world_ops.h` (**69** Milestone 4 handlers across input, ray/LOS, strings, troops, parties, and `53`/`2180`/`2749`), `native_world_ops_2.h` (**69** Milestone 4 handlers across mission/teams, audio/particles/tableau, and game_state_and_stores), `native_world_ops_3.h` (**80** Milestone 4 `other_native` handlers; **218** new Milestone 4 switch opcodes total plus `3` Milestone 3 upgrades `1802`/`1930`/`1969`, completing 16/20 categories to 100%), `native_agent_ops.h` (**50** Milestone 5 `agents` handlers + `4` upgraded cases = `54` cases), `native_mp_ops.h` (**48** Milestone 5 `multiplayer_and_players` handlers), `native_scene_ops.h` (**56** Milestone 5 `scenes_props_and_mission` handlers), and `native_ui_ops.h` (**62** Milestone 5 `ui_presentations_and_menus` handlers + `5` upgraded cases = `67` cases; **216** new Milestone 5 switch opcodes total plus `9` upgraded cases, completing all **20/20** native categories to 100%).
  - **6** shared `noop_true` stubs jumping directly to `0x1401e226b` (`473`, `477`, `1070`, `1104`, `1302`, `1491`, handled cleanly in `dispatch_native_non_switch`).
  - **1,695** `execute_wse_fallback` slots jumping to `0x1401e223e`: **1** SDK-named (`1004` `race_completed_by_player` / `is_vanilla_warband`, returning `false` via `dispatch_native_non_switch`) and **1,694** unassigned holes.
- Unimplemented native and WSE opcodes return explicit `NativeDispatch::unimplemented` / `WseDispatch::unimplemented` states recorded in `VmContext::unimplementedOpcodeCounts` (as well as `unimplementedCount` / `lastUnimplementedOpcode`). Compatibility mode (`failOnUnimplemented = false`) falls through `true`; strict/2H harness mode (`failOnUnimplemented = true`) fails loudly with an error and aborts execution before `else_try` / `neg` / `this_or_next` / `call_script` can mask the hit (`ROADMAP.md` B6).

### Demand ranking (`tools/demand_rank.py`, Backlog B3)

Measured on the `3,467,512`-byte retail `Native` `scripts.txt` (**1,755** scripts, **131,080** statements, **653** distinct opcodes; counts only — no module content; `triggers.txt` and `simple_triggers.txt` were absent from that local build output):

- **Covered today:** `131,080` statements (**100.00%** — `131,028` implemented native calls, `24` `noop_true`/fallback calls, `28` implemented WSE calls; `0` entity-iterator stub calls). `1,755 / 1,755` (**100.00%**) scripts use zero unimplemented opcodes.
- **Unimplemented demand:** `0` native calls across **0** distinct native opcodes, plus `0` WSE calls (`0` distinct opcodes).

All **20/20** native categories are 100% covered in `scripts.txt`.

### Tests

| Suite | Test functions |
| --- | ---: |
| `operand_test.cpp` | 6 |
| `trigger_test.cpp` | 10 |
| `savegame_test.cpp` | 18 |
| `script_test.cpp` | 5 |
| `wse_bitwise_fp_test.cpp` | 11 |
| `wse_system_test.cpp` | 8 |
| `wse_string_test.cpp` | 3 |
| `wse_dict_test.cpp` | 3 |
| `wse_array_test.cpp` | 2 |
| `wse_world_test.cpp` | 5 |
| `block_vm_test.cpp` | 22 |
| `native_world_test.cpp` | 7 |
| `native_m5_test.cpp` | 5 |
| `wse_m6_test.cpp` | 7 |
| `platform_test.cpp` | 16 |
| `module_loaders_test.cpp` | 6 |
| `headless_test.cpp` | 13 |
| `lua_test.cpp` | 7 |
| `asset_test.cpp` | 10 |
| `audio_test.cpp` | 9 |
| `input_test.cpp` | 8 |
| `scene_test.cpp` | 6 |
| `ui_test.cpp` | 17 |
| `recomp_test.cpp` | 5 |
| `recomp_llvm_test.cpp` *(with `-DWSE_ENABLE_LLVM=ON`)* | 1 |
| `selftest_host_test.cpp` *(aggregate runner over the 24 suites)* | 24 suites |
| **Total (24 standalone suites + aggregate runner)** | **210** *(219 `test_*` functions measured across `*_test.cpp`)* |

## 4. Blocked

| Blocker | Impact | Way out |
| --- | --- | --- |
| No local Android NDK/SDK, `adb`, or device | CI runs the NDK arm64 compile gate on Linux; on-device Android runtime tests remain unrun | Keep `.github/workflows/ci.yml` enforcing the arm64 build; add `adb`/device validation later in Phase 3/4 |
| REA not registered with OpenCode | REA/Ghidra tools are not exposed through MCP; the CLI works from `%TEMP%\opencode\tools\rea\node_modules\rea-agents` | Keep using the CLI |

## 5. Next concrete tasks (ordered)

1. **Remaining Phase 2 work (`ROADMAP.md` §Phase 2):** Phase 2A whole-savegame framing & `[802..1174]` versioned section codecs (`wse/game/savegame.h`, `wse/io/binary_stream.h`), Phase 2C `mbSimpleTriggerManager::execute` early-out + `jump_to_menu`/`consume_campaign_queued_window` lifecycle (`wse/vm/block_vm.h`), Phase 2E live `party`/`agent`/`mission_object`/`player` containers and lifetimes (`wse/game/world_state.h`), Phase 2F Lua 5.1 (`WSELuaManager` + stock Lua 5.1.5), Phase 2G shared `RglString` (`wse/core/rgl_string.h`) + `.brf`/`.dds` decoders (`wse/asset/{brf,dds}.h`), and Phase 2H headless `Native` execution are complete on host. Remaining Phase 2 core items are the broader `2D` WSE/engine-backed subfamilies and remaining standalone module format coverage under `2A`.

Platform track, in parallel (`docs/PLATFORM_PLAN.md`):

1. ~~**P5 GLSL → SPIR-V translator:**~~ **Done:** `emit_placeholder_spirv_module` has been replaced by `lower_warband_glsl_to_vulkan450` (`wse/asset/shader.h`) + Khronos `glslang 16.6.0` (`wse/asset/shader_compiler.h`, `wse_shader`), with `SpirvShaderCache` (`kWseSpirvGeneratorVersion = 4`) persisting validated SPIR-V 1.0 binaries plus `.refl` reflection sidecars. Re-verified `271/271` retail Warband techniques (`Native` & `Phantasy Calradia`) at generator `4` (`spirv_fail = 0`) and clean Windows `build_host.ps1` (`aa754f6`).
2. ~~**P8 Sensors + performance (SDL3 / Android):**~~ **Host-verified (device run pending hardware):** added `wse/platform/performance.h` (`FrameRateCap` 60/90/120 Hz + uncapped, `PresentModePreference`, hysteretic `ThermalPerformanceController`, drift-free `FramePacer`, `RuntimePerformanceHub`, persistent `Vfs` JSON settings), opt-in low-pass filtered `Mission` gyro aiming + `CampaignMap` two-finger pinch-to-zoom (`wse/platform/input.h`, `wse/game/world_input.h`), `SDL_INIT_SENSOR` `SDL_SENSOR_GYRO` (`platform/sdl/sdl_input.{h,cpp}`), Android `PowerManager` thermal listener/`getThermalHeadroom` + `ANativeWindow_setFrameRate*` + SDL3 pacing (`sdl_selftest_main.cpp`, `saf_vfs_jni.cpp`, `WseActivity.kt`, `WseSafBridge.kt`), and persisted launcher toggles (`WseLauncherActivity.kt`).
3. **P9 Convergence with Phase 2 & device verification (`arm64-v8a` APK):** the app's frame loop now drives `UiSession` and renders it with `UiRhiRenderer` through `platform::UiGameShell` (`--module`, the `wse.module_root` intent extra, or the launcher's "Run Selected Module" button for SAF-picked installs; open item 1 of the P9 Part 2 review list below has what is left, chiefly a device run); then scene/terrain/skeletal rendering, widgets/presentations, Bullet physics, plus installing `wse2-android-selftest-debug-apk` on an `arm64-v8a` device to record live SAF timing, SDL3 audio playback, touch/gamepad/gyro/pinch controls, 60/90/120 Hz frame pacing + thermal throttling, and real Turnip/PanVK/System Vulkan probe results.

4. **P10 Launcher UI/UX and performance on Android (host-verified; device run pending):** all units P10.0-P10.8 landed (`docs/PLATFORM_PLAN.md` "P10"). Launcher: Home / Modules / Settings / Advanced pages (`android/.../launcher/`), dark theme through `LauncherUi` and `res/values/{colors,dimens,themes}.xml`, folder scan with progress and cancel, a per-module install check (`install_check_json`, `nativeCheckModuleInstall`) gating Play, grouped settings (`quality_preset`, `render_scale`, `max_texture_size`, `ui_aspect`, `ui_scale`, `language` in `config/performance.json`; `ensure_first_launch_settings` writes the Medium preset on first launch; `save_launcher_settings_json` saves partial changes), languages from the install (`list_warband_languages`). Runtime: `UiGameShell::start_async` with a loading screen (SAF indexing and the load on JVM-attached worker threads, `RegisteredGameTree::rebind_thread`), module mode following `RuntimePerformanceHub` (`PerfLoopState`, shared with the self-test loop) plus a `--show-fps` overlay, a texture budget (`select_budget_mip`, `decode_dds_rgba8_budgeted`, font atlas exempt), and an ADPF performance-hint session (`platform/android/perf_hint.{h,cpp}`, thread-priority fallback). Next: the user's phone check of the launcher and module start, then P11 (in-game UI and touch controls). Known gaps: SAF indexing cannot be cancelled mid-walk; nothing calls `set_present_capabilities` yet, so module mode presents FIFO (as the self-test loop did).
5. **P11 / P12 / P4 wave A landed, wave B in progress (platform):** P11.1 aspect modes, P11.3/P11.4 drawn touch overlay and context buttons, P12.1 Compose + Material 3 on a `LauncherViewModel`, and P4 `VulkanRhiDevice` GPU rendering (`12,288/12,288` pixels within 8/255 of `SoftwareRhiDevice` on the host's Intel HD 520 with `WSE_TEST_SYSTEM_VULKAN=1`) are host-verified (`docs/PLATFORM_PLAN.md` "P11", "P12"). The module shell now draws and routes the overlay (`UiGameShell::step_input`, layout clamped to `SDL_GetWindowSafeArea`); the shell reaches only menu, presentation and map screens today, so the overlay shows on the map screen only until missions and conversations render. Wave B landed too (P11.2 filter + text scale, P11.5/P11.6 touch menus, scissor and soft keyboard, P12.2 module art, P12.3/P12.4 Compose screens; host-verified, the Kotlin and JNI compiled only by CI). The user's phone check of wave B passed (2026-11-18); the look was judged flat, so P13 (modern medieval art direction, `docs/PLATFORM_PLAN.md` "P13") followed: P13.0-P13.2 landed with CI screenshots, P13.3 (screenshot-review fixes and motion) is next; Vulkan presentation in the app waits for a phone check of the GPU render path.
   - **UI not finished (user, 2026-11-18):** the launcher (P10, P12, P13) and the in-game UI (P11) are not final. Treat their layout, styling and behaviour as subject to change during Phase 5; the recompiled game brings its own UI needs (`docs/RECOMP_PLAN.md`), so do not treat the current screens, component kit or touch overlay as frozen contracts.
6. **Phase 5 R0 feasibility spikes (built; CI and phone run pending; `docs/RECOMP_PLAN.md` "R0"):**
   - **Built:** header-only core under `engine/include/wse/recomp/` (`x86_decode.h`, `x86_interp.h`, `pe_image.h`, `r0_inputs.h`, `x86_synth.h`) and host suite `recomp` (`engine/tests/recomp_test.cpp`, registered in `wse_selftest.{h,cpp}`, CMake, and `engine/build_host.ps1`, passing on Windows); `wse_recomp` (`engine/src/recomp/{lifter_llvm.cpp,r0_spikes.cpp,recomp_llvm.h,r0_spikes.h}`) and `recomp_llvm_test` (CTest `recomp_llvm`, built with `-DWSE_ENABLE_LLVM=ON` against LLVM 18 + LLD); Android `libwse_r0.so` (`platform/android/r0_jni.cpp`) wired to `android/app/src/main/kotlin/org/wse2/android/launcher/R0Spikes.kt` (Advanced screen `"R0 spikes"` row); CI jobs `recomp-host` (`llvm-18-dev` + `liblld-18-dev`) and `llvm-android` (builds LLVM + LLD `18.1.8` for `arm64-v8a` once and caches it, required by `android-apk`, which checks `libwse_r0.so` is in the APK); and local-only Windows PC reference `tools/r0_pc_reference.cpp` (never run in CI — maps the user's v1.1.5.5 exe at its preferred base and checks `rglString::indexOf` `0x14030f630` over 512 cases: native == interpreter == model, FNV-1a hash `0x7aeac64f5cbff537` on 2026-11-18).
   - **CI:** all jobs green on `413e95e` (run `44336216717`); R0 (c) APK cost measured there: `libwse_r0.so` (LLVM + LLD + translator, AArch64 only) is 32,103,368 B uncompressed / 13,945,355 B compressed, and the debug APK grows from 28,396,955 B to 42,416,067 B.
   - **Phone run (2026-11-19, user's phone, 8 cores, `aarch64-unknown-linux-android29`, APK 42.4 MB):** **(a) PASS**: LLVM emits the probe in 20.0 ms and LLD links it in 5.7 ms; `dlopen` of the read-only file, `dlopen` from a `memfd` and our own ELF loader all run it. **(b) PASS**: 8,232,960 B mapped at the image base. **(c) measured**: the 42,003-byte synthetic function (14,002 instructions) takes 6,459 ms at O2 (lift 183, opt 4,007, codegen 2,265, link 3 ms; about 6 KB/s) and 6,938 ms at O1, translated == interpreter at both; whole `.text` extrapolated to 806 s on 1 core, about 101 s on 8. **(d) skipped**: the phone install carries WSE2 **v1.1.5.6** (`mb_warband_wse2_x64.exe` 7,066,112 B, SHA-256 `fbe9808c…`, PE timestamp 2026-11-15, 14,644 `.pdata` functions; the same file as the PC's GOG folder), not the pinned v1.1.5.5 (`WSE2.zip`).
   - **Pending:** (d) on the phone with the v1.1.5.5 exe (see the decision in `docs/RECOMP_PLAN.md` "The target"). R1–R9 follow in that document's order.

### P4 review: items not fixed yet

Found in the review of `ef4f3e0` and still open after the second round of fixes (`STATUS.md` §7,
"Slice P4 second review fixes"). Each needs hardware, a GPU pipeline, or a design decision this
environment cannot make.

1. **The P4 "render test" never uses the GPU.** `VulkanRhiDevice` creates `VkBuffer` / `VkImage`
   (and, since P5, `VkShaderModule`) objects, but binds no memory, uploads no data, records no
   command buffer, and draws every frame with `SoftwareRhiDevice`. The frame hash is therefore
   identical for every driver (the tests assert exactly that) and the probe only proves
   `vkCreateDevice` worked. Way out: allocate and bind memory, upload through a staging buffer,
   build a real render pass + graphics pipeline from the P5 SPIR-V modules, read back, and
   compare against the CPU reference within a tolerance; add a CI job
   on Mesa lavapipe (`mesa-vulkan-drivers`) as `docs/PLATFORM_PLAN.md`'s original P4 exit asked,
   run with `WSE_TEST_SYSTEM_VULKAN=1`.
2. **On-device behaviour is unverified.** The isolated `:vkprobe` probe process
   (`WseVulkanProbeService`), its crash/timeout reporting, the `android_get_device_api_level()`
   `minApi` check, and the requested-mode check in `nativeStageDriverPackage` are compile-checked
   only (Kotlin against the Android 14 framework jar, JNI on the host). Needs a device (§4).
3. ~~**Windows changes are compile-checked only.**~~ **Verified on Windows:** `LoadLibraryExA(..., LOAD_LIBRARY_SEARCH_SYSTEM32)` for `vulkan-1.dll`, UTF-8 custom-driver paths via `LoadLibraryExW`, `HostVfs::rename_file`, and the composed `1.0` + WSI-refusal fallback loop in `driver_loader.cpp` now build and pass in `engine/build_host.ps1` (`platform_test.cpp`).
4. ~~**Front face of real Warband meshes.**~~ **Resolved:** `rglCamera::getProjectionMatrix` (`0x14025ac20` / `D3DXMatrixPerspectiveOffCenterRH` `0x140501b60`) + `rglRenderer::renderMesh` (`0x1402e8ccd`) + retail `CommonRes/shields.brf` CCW winding + `SoftwareRhiDevice` + `make_orbit_mvp_matrix` (`platform_test.cpp`) confirm `FrontFace::counter_clockwise`; `make_pipeline_desc` (`wse/platform/rhi_material.h`) updated to match.
5. **Software rasterizer throughput.** Remaining ideas, deferred because the GPU path replaces this
   rasterizer for real frames: 8x8 tile binning with empty-tile skipping and one thread per tile
   row; integer power-of-two texture wrap (`& (w - 1)`); a word-at-a-time hash (xxh3) for large
   frames (`hash_rgba8_frame` is a regression contract, so changing it changes every recorded hash).
6. ~~**`SafVfs` has no `rename_file`.**~~ **Fixed:** `SafTreeBackend::rename_document` / `move_document` (default unsupported) back `SafVfs::rename_file`. `MemorySafBackend` implements both, as does the Android JNI backend (`WseSafBridge.renameDocument` / `moveDocument` over `DocumentsContract`, API 24, covered by minSdk 29; compile-checked only, device pending). An existing target is deleted first because SAF has no atomic replace, so a move or rename that fails after that delete leaves the target missing (the source stays). Backends without support still return false, so callers keep the remove + write fallback.
7. **Non-ASCII directory case.** `HostVfs` cache keys fold ASCII only, so two spellings of a
   non-ASCII directory that differ only in case (on a case-insensitive volume) get separate
   listings: correct, one extra scan each. Unicode case folding was not worth a dependency.

### P5 review: items not fixed yet

Found in the review of `6be524d` and still open after the fixes in §7 ("Slice P5 review fixes").
Each needs a translator choice, the retail PDB/binary, hardware, or a change to pushed history.

1. ~~**No GLSL → SPIR-V translator.**~~ **Resolved (`glslang 16.6.0`):** `emit_placeholder_spirv_module` was removed; `lower_warband_glsl_to_vulkan450` (`wse/asset/shader.h`) + `create_glslang_shader_compiler()` (`wse/asset/shader_compiler.h`, `engine/src/shader/wse_shader_compiler.cpp`) lower Warband GLSL 1.10/1.20 to `#version 450` and compile into Vulkan SPIR-V 1.0, tagged by a Section 7(a) `OpString` checked by `inspect_spirv_module` and by `spirv-val --target-env vulkan1.0` in CI. The lowering itself was reworked after review (§7 "Slice P5 lowering review fixes"; behavior notes in `engine/README.md`). All `271/271` retail Warband techniques compiled at generator `3`; P5 is ticked `[x]` in `ROADMAP.md`.
2. ~~**Unconfirmed `rglMaterial` decoding.**~~ **Resolved in v1.1.5.5:** `rglMesh::updateRenderOrder`
   (`0x1402b9e50`, `0x1402b9ef5`) computes `((flags >> 24) - 8) & 0xF` (+4 bucket when blended),
   confirming signed nibble `-8..+7` for `render_order`. `rglGraphicsContext::createBlendStates`
   (`0x14029f590`) allocates slots `0..4`; values `5..6` have no slot, while `7` (`0x700`)
   selects slot `1` (`AlphaBlend`) vs `0` (`Opaque`) dynamically per draw from vertex-color alpha
   (`0x1402e913f` / `0x1402b9ece`). `rglRenderer::renderMesh` / `setAlphaTestMode` (`0x1402e8439` /
   `0x1402a00b0`) keep `D3DRS_ALPHATESTENABLE`/`D3DRS_ALPHAREF` active even on blended materials,
   `shader->m_flags & kShaderFlagHasSpecular` (`0x20`, `0x1402e8454`) gates material specular, and
   DX9 sets no `D3DSAMP_SRGBTEXTURE`/`D3DRS_SRGBWRITEENABLE` (`rglMesh::gammaCorrect` `0x1402c02b0`
   handles gamma in shaders, so `srgb_diffuse = !diffuse_a.empty()` is appropriate for Vulkan
   `R8G8B8A8_SRGB`).
3. ~~**`make_pipeline_desc` assumes clockwise front faces**~~ **Fixed:** updated
   `wse/platform/rhi_material.h` to `FrontFace::counter_clockwise` (verified by `v1.1.5.5`
   `0x1402e8ccd`/`0x14025ac20`, retail `CommonRes/shields.brf`, and the CCW orbit-camera
   regression test in `platform_test.cpp`).
4. ~~**Retail technique count.**~~ **Resolved:** fixed `kWse2SkinMappings[0]` base typo
   (`"faceshader_high"` → `"face_shader_high"`) in `wse/asset/shader.h` and `asset_test.cpp`;
   fresh `wse_brf.exe` records `gl_techniques=271` (`0` warnings, all 5 WSE2 variants incl.
   `face_shader_skin_high` synthesized), `Native` (`221/221`, `144` shaders [`111` suitable,
   `33` fallback], `633/633` resolved materials, `48` alpha-tested, `261` blended, `58` INI
   pairs) and `Phantasy Calradia` (`333/333`, `144` shaders [`111` suitable, `33` fallback],
   `1,963/1,963` resolved materials, `127` alpha-tested, `430` blended, `58` INI pairs).
5. **Lighting is still an approximation.** `MeshVertex` carries no tangents, so the software
   rasterizer derives one flat UV tangent per triangle (no smoothing across vertices), and the
   key light, ambient term, and Blinn-Phong model are port choices, not retail shader behaviour.
   The GPU path with translated shaders replaces this.
6. **`docs/SYMBOL_MAP.json` churn and date.** `6be524d` re-sorted the `strings` list (a
   17k-line diff for 45 new records) and left `"generated": "2026-11-15"`. Fixing either means
   regenerating with `tools/build_symbol_map.py`, which needs the v1.1.5.5 PDB/PE not present in
   cloud sessions; never hand-edit the file.
7. **`6be524d` author `Dev <dev@localhost>`.** The commit is pushed and merged into this branch;
   correcting it would rewrite shared history. Set `git config user.name/user.email` on that
   machine for future commits.
8. **Doc citation baseline.** `tools/docs_citation_baseline.txt` tolerates `36` pre-existing doc VAs
   that `SYMBOL_MAP.json` does not curate (image base `0x140000000`, `.data` globals such as
   `0x140617819`, labeled historical/superseded sections). Curating them (as `global_objects` or
   strings) would let the baseline shrink; and because the checker accepts any VA inside a known
   function, a mislabeled interior VA still passes — only review catches that.
9. **CI and device runs (Windows now verified).** After resolving the P4/P5 review follow-ups
   (`face_shader_high` base, `kShaderFlagHasSpecular` `0x20`, `alpha_ref_u8` on blended materials,
   `FrontFace::counter_clockwise`, composed `1.0` + WSI refusal fallback loop in
   `driver_loader.cpp`, `rhi_brf.h` `>= 65536` vertex guard, `saf_vfs_jni.cpp` display-name
   control-char sanitization, and `WseLauncherActivity.kt` atomic ICD backup swap), Windows
   `engine/build_host.ps1` + retail corpus (`WSE_WARBAND_ROOT`) + `wse_brf.exe` +
   `check_citations.py --self-test --docs` PASS. Android NDK/APK CI jobs and an on-device WSI
   probe (`VK_KHR_swapchain` enablement) remain pending CI/hardware.

### P6 review: items not fixed yet

Found in the review of `f0b4f19`; the bugs it found are fixed (§7, "Slice P6 review fixes").
These remain because they need retail evidence, hardware, or a design decision.

1. **Audio is only partly wired (partly fixed).** `wse_headless --audio` (`HeadlessRunOptions::drive_audio`) attaches a `WorldAudioBridge` before `game_start`, feeds the listener from the camera each frame (`listener_from_camera_transform`), calls `WorldAudioBridge::update`, and pumps `pump_to_latency` into a `CaptureAudioDevice`; `HeadlessReport::audio_stats` / `audio_frames_mixed` report the result. It decodes WAV only unless a caller passes `audio_codecs`. The SDL / Android app now keeps its SDL3 stream open with a mixer on `AudioMixerWorker`, and its lifecycle event watch pauses / resumes it (`SDL_EVENT_WILL_ENTER_BACKGROUND` / `SDL_EVENT_DID_ENTER_FOREGROUND` -> `set_paused`). Still open: the app runs no game world, so no `WorldAudioBridge` or `create_audio_codec_backend()` is attached there; device verification needs hardware.
2. **Decoding and mixing run on the caller's thread (partly fixed).** `--preload-audio` (`HeadlessRunOptions::preload_audio`) decodes every sample at load, so no play opcode waits on a file read and decode. **Fixed:** `AudioMixerWorker` (`wse/platform/audio_worker.h`) mixes on its own thread behind one mutex; game-thread mixer and bridge calls take the same mutex (`lock()` / `with_mixer`), and channel-end callbacks run on the worker thread under it (`audio_test.cpp` `test_audio_mixer_worker_thread`). A lock-free command queue was not needed at this load; revisit if lock contention shows up on device.
3. ~~**Unverified retail details.**~~ **Resolved in v1.1.5.5:** `mbAgent::playSound` (`0x140057010`)
   hashes `m_no`, `m_troopNo`, and `m_partyNo` (`WseAgent::party_no`), scales `(acc/871 - 0.5f)`
   by `0.3f` (not `0.14f`), applies `1.0f / (1.0f - delta)` for negative `delta < -0.001f`, and
   skips pitch variation for the player agent (`wse/asset/audio.h` and `native_mission_ops.h`
   updated). `mbSoundManager::setChannelPropertiesAndPlay` (`0x140332d80`) has no random frequency
   jitter (`kSoundFlagDisableFreqVariation` `0x8` gates `m_frequencyMultiplier` and streaming) and
   uses `rand15() % m_length` (the mixer keeps `(draw * frames) >> 15` as an intentional whole-sample
   improvement). `mbSoundManager::initializeFmod` (`0x140332f80`) maps `bDistanceFilter` to
   `FMOD_INIT_DISTANCE_FILTERING` (`0x200`) and `bHrtfFilter` to `FMOD_INIT_HRTF_LOWPASS` (`0x10`)
   while 3D inverse rolloff (`2.0f..80.0f`) is always active (`distance_filter` in `AudioConfig`
   still toggles rolloff and `hrtf_filter`/Doppler remain unimplemented). Also fixed re-entrant
   `flush_channel_ends()` sibling-slot reuse canceling stale queued end notifications in
   `wse/platform/audio.h`.
4. **No pinned mix hash.** The determinism test still compares two runs with each other; a golden
   hash would differ between glibc, Bionic and MinGW `libm` (`sin`/`cos`/`exp`/`tanh`), so the
   regression tests pin exact sample values on paths that avoid those functions instead.

### P7 review: items not fixed yet

Found in the review of `4793c11`; the bugs it found are fixed (§7, "Slice P7 review fixes").
These remain because they need retail evidence, hardware, or a design decision.

1. ~~**Chords do not exclude extra modifiers.**~~ **Resolved in v1.1.5.5:** `mbGameKeyManager::keyDown` (`0x1400eedf0`), `keyClicked` (`0x1400eee90`), and `keyRaised` (`0x1400eef50`) loop only over the target slot's `m_numModifiers` without checking or suppressing longer chords on the same main key (`chord_matches` matches retail directly).
2. ~~**Voice-pitch clamp has no evidence.**~~ **Resolved in v1.1.5.5:** `mbAgent::playSound` (`0x140057010`) computes `delta = (rem / 871.0f - 0.5f) * 0.3f` and branches on `±0.001f` with no `±0.15f` clamp; removed `std::clamp(..., -0.15f, 0.15f)` in `wse/asset/audio.h` (`audio_test.cpp` regression on negative wrapped sum `(-29, 0, -1)` → `0.8615659`).
3. ~~**Two camera-frame conventions.**~~ **Resolved in v1.1.5.5:** `0x140332970`, `0x1402ff7b0` (`rglCamera` project), and `0x1402ff940` (`rglCamera` unproject) confirm Warband's `rglTransform` camera frame is `s` = right, `f` = up, `-u` = forward, and `0x1402ff7b0` projects into Warband UI coordinates `[0, 1] x [0, 0.75]` (y-up); fixed `position_get_screen_projection` (`750`, `0x1401cbc3c`, `native_world_ops.h`) and `mouse_get_world_projection` (`751`, `native_ui_ops.h`) plus a `750 <-> 751` round-trip test in `native_m5_test.cpp`.
4. ~~**`omit_key_once` also hides the key from scripts.**~~ **Resolved in v1.1.5.5:** `0x1401c69b5` (`omit_key_once`) marks keys for `rglInputManager::updateOmittedKeys`, which forces `prev[k] = 1` while Warband's default-action handlers run and restores saved `prev` before module scripts execute, whereas `70 key_is_down` (`0x1401c67e5`) and `71 key_clicked` (`0x1401c683f`) read live `cur`/`prev`. Stopped filtering `70`/`71` by `omitted_keys` in `native_world_ops.h` (`72`/`73` game-key ops keep suppression), cleared `suppressed_keys_` on same-frame `KeyUp` + `KeyDown` in `wse/platform/input.h`, and updated `native_world_test.cpp` + `input_test.cpp`.
5. **SDL backend not run on a device.** Finger cancel, focus loss / background, Android Back → Esc,
   flipped wheel, and per-event window sizing compile against SDL `3.4.20` headers and the core
   logic is host-tested, but nothing has exercised `sdl_input.cpp` on hardware. Gamepad Guide and
   paddle buttons stay unmapped (no retail raw key in `0x1402ad360`).
6. **Touch-control design work (not bugs, not started):**
   - The attack zone presses `Attack` on touch-down, before a swipe direction exists, and nothing
     turns `attack_dir` / `block_dir` into the mouse movement Warband reads the direction from.
     Delay the press until the swipe is classified (tap = default direction) and feed the
     direction into the look delta.
   - The command radial is single-level (`Order1..8` = F1..F8), so sub-orders such as "Hold this
     position" (F1 → F1) are unreachable, and the `ViewOrders` pulse on every open may toggle the
     order panel closed. Needs a two-level radial (group → order).
   - Default widget positions ignore the display safe area (notch / rounded corners); no haptics,
     no floating stick re-centred on the thumb, no overlap warning in `TouchLayoutEditor`, and no
     per-device default profile alongside the per-module one.
7. **History of `4793c11` itself.** It was pushed straight to `main` as author `Dev <dev@localhost>`,
   its message says "Merge P6 review commit 92fcab9" although it has one parent, and it bundles the
   P6 follow-ups with the whole P7 slice. Rewriting `main` is not worth it; noted for future commits.

### P5 lowering review: items not fixed yet

Found in the review of `6b85ed7`; the bugs it found are fixed (§7, "Slice P5 lowering review
fixes"). These remain because they need the retail install, a Windows host, hardware, or a design
decision.

1. ~~**Retail corpus not re-run at generator `4`.**~~ **Verified on `aa754f6` (`generator 4`):** `wse_brf.exe --compile-shaders --json` with `WSE_WARBAND_ROOT` compiled `271/271` techniques (`spirv_fail = 0`) on both `Modules/Native` (`221/221` loaded, `633/633` materials resolved) and `Modules/Phantasy Calradia` (`333/333` loaded, `1,963/1,963` materials resolved).
2. ~~**Windows host gate not run.**~~ **Verified on `aa754f6`:** cleanly deleted and rebuilt `libwse_glslang.a` (the trimmed 40-file `glslangFiles` list in `engine/build_host.ps1`) and passed all `21/21` standalone suites + `selftest_host` (`21/21`, `7414 ms`) + `wse_headless` + `wse_brf`.
3. ~~**Compatibility built-ins are not lowered.**~~ **Mostly done (§7 "Slice P8 review
   fixes"):** `gl_Vertex` / `gl_Normal` / `gl_Color` / `gl_SecondaryColor` / `gl_FogCoord` /
   `gl_MultiTexCoord0..7`, `gl_FrontColor` / `gl_FrontSecondaryColor`, `gl_TexCoord[]`,
   `gl_FogFragCoord`, the matrix uniforms (`gl_ModelViewProjectionMatrix`, `gl_NormalMatrix`,
   `gl_TextureMatrix[]`, inverses / transposes), and `ftransform()` now lower. Still missing:
   the fixed-function state structs (`gl_LightSource[]`, `gl_Fog`, `gl_FrontMaterial`, ...) and
   `gl_BackColor`; and nothing uploads the `gl_*` matrices yet (same uploader gap as item 5).
4. **Clip-space convention unverified.** Lowering leaves `gl_Position` as written, so the
   matrices uploaded later must already be Vulkan-style (Y down, depth `[0, 1]`;
   `wse/platform/rhi.h` has the flip helper). Needs a real Vulkan draw on a device.
5. **Uniform defaults are reported, not applied.** `ShaderStageReflection::uniform_defaults`
   carries `uniform T x = ...;` initializers, but no uniform uploader exists yet to seed them.
6. **`shadow2D` result assumes `GL_LUMINANCE` depth mode.** The `_wse_shadow2D*` helpers return
   `(r, r, r, 1)`; retail may set `GL_DEPTH_TEXTURE_MODE` to `INTENSITY` or `ALPHA`. Needs the
   retail GL renderer's state.
7. **Single-stage callers.** Lowering or compiling one stage without `partner_glsl` still lets
   names outside the fixed tables land on different slots in the two stages. The API cannot
   enforce pairing; every in-tree caller passes the partner.
8. **Raw-path limits.** `lower_warband_glsl_to_vulkan450` on unpreprocessed source (no backend
   preprocessing) emits declarations inside `#if` blocks unconditionally and moves top-level
   `#define`s ahead of them; `#version 4xx` sources with loose uniforms pass through unlowered.
9. **Internal glslang header.** `wse_shader_compiler.cpp` uses `glslang/Include/Types.h` (sampler
   type and descriptor set in reflection), which distro packages such as Ubuntu's `glslang-dev`
   do not install, so only the vendored tree builds it. Reading set/binding from SPIR-V
   decorations would remove the dependency.
10. **glslang is always built.** Every configure downloads it from `github.com` (or needs
    `WSE_GLSLANG_SOURCE_DIR`), and it links into the self-test APK; there is no CMake option to
    leave it out and the APK size impact is unmeasured.
11. ~~**No parallel warm-up in the runtime yet.**~~ **Fixed:** `SpirvShaderCache::warm_up`
    probes the cache on the calling thread, compiles the distinct misses on up to `jobs` threads
    sharing the backend, then writes the modules and counters on the calling thread
    (`asset_test.cpp`). Still open: no runtime renderer calls it yet. Original note: `wse_brf --compile-shaders` now compiles on
    `--jobs N` threads (default: all cores), but the runtime `SpirvShaderCache` still compiles on
    first use, one stage at a time; `SpirvShaderCache` itself is not thread-safe (counters,
    `last_error`), so a warm-up pass must probe/store on one thread and compile on many.

### P8 review: items not fixed yet

Found in the review of `9411644`; the bugs it found are fixed (§7, "Slice P8 review fixes").
These need a device or a design decision.

1. **Gyro axis remap unverified on hardware.** `gyro_look_rates` follows Android
   `remapCoordinateSystem` and SDL's `SDL_ORIENTATION_*` -> `Surface.ROTATION_*` mapping (checked
   against SDL `release-3.4.8` `SDL_android.c`), and subtracts the natural orientation for
   natural-landscape tablets, but sign conventions need a phone in both landscape directions.
   Controller gyro axes (`SDL_EVENT_GAMEPAD_SENSOR_UPDATE`) are likewise untested.
2. **`ANativeWindow_setFrameRateWithChangeStrategy(..., ALWAYS)`** is used for the launcher's
   choice so a 90/120 Hz cap can switch display modes; on some panels that switch blanks the
   screen briefly. Needs a device to judge; thermal step-downs keep `ONLY_IF_SEAMLESS`.
3. **Zoom bounds resolved in `v1.1.5.5`; live campaign camera still unrendered.** ~~`20` and `[1, 200]` placeholders until `mbBasicGame` `+0x5050` limits are read from the binary.~~ Verified in `v1.1.5.5`: `mbMapWindow::mbMapWindow` (`0x140116d70`) initializes `+0x2d0` (`m_cameraDistance`) to `5.0f`, and `rglConfig::Campaign::fMapCameraMinZoom` (`0x1406173c0`) / `fMapCameraMaxZoom` (`0x1406173c4`) bound zoom to `[3.5f, 175.0f]`. `WseInputState` and `WorldInputBridge::sync_to_world` now use `5.0f` and `[3.5f, 175.0f]` (`input_test.cpp`); no campaign-map camera renderer reads `map_camera_distance` yet.
4. ~~**Two settings sources.**~~ **Fixed:** `config/performance.json` (under the app's
   `filesDir`) is the one saved copy. The launcher reads and writes it through JNI
   (`nativeLoadPerformanceSettings` / `nativeSavePerformanceSettings`, `save_launcher_choices`),
   now also edits present mode, dynamic resolution and LOD-bias scaling, and migrates the old
   `SharedPreferences` keys once; `WseActivity` loads the file into `RuntimePerformanceHub`
   (`nativeApplyPerformanceSettings`) instead of reading intent extras. Device check pending
   (Kotlin and JNI compile in CI only, item 5).
5. **Kotlin compile-checked in CI only.** The `WseActivity.getLibraries()` restore, the
   refresh-rate filter and gyro label in `WseLauncherActivity`, and `SystemClock.elapsedRealtime`
   polling build only in the `android-apk` job (no Android SDK in cloud sessions).

### Phase 2A savegame review: items resolved in `v1.1.5.5`

Found in the review of `9cc4431` (merged as `ca0163a`); all 7 follow-up items are now verified against `v1.1.5.5` and resolved:

1. ~~**`mbNotes` per-note fields.**~~ **Verified (`0x1401b9e90` / `0x1401b9f30`):** Disassembly of `mbNotes::readStream` and `mbNotes::writeStream` confirms `m_tableauMaterialNo` (`+0x4c0`) and `m_available` (`+0x4c4`) are scalar object-level fields loaded/written across the 16-entry loop alongside per-note `m_texts[i]` (`+0x40`, stride `0x48`) and `m_dates[i]` (`+0`, stride `4`). `savegame.h`'s `write_notes`/`read_notes` already match retail.
2. ~~**Pre-1174 body layouts.**~~ **Resolved (`[802..1174]`):** Audited `mbSavegame::readStream` (`0x1400c73b0`), `mbGame::loadQuests`..`loadItemKinds`, `mbEvent::readStream` (`0x1400bae70`), and trigger managers: across `[802..1174]`, all other readers are version-invariant and only four `g_compatibleSavegameVersion` branches exist (`mbGame::readStream` `0x1400c856d` combat options `> 1136`; `mbParty::readStream` `0x1401ba9d1` `is_marshall` `(900..999) || >= 1020`, `0x1401baa20` `extra_icon_*` `> 1136`, `0x1401baa7f` `ignore_with_player_party` `> 1160`). Lowered `kSavegameOldestReconstructedBodyVersion` to `kSavegameMinVersion` (`802`), gated all 4 branches in `savegame.h`, and tested round-trips across `{802, 950, 1000, 1020, 1136, 1160, 1174}` (`savegame_test.cpp`).
3. ~~**`toInt64Stream` / `toUInt64Stream` semantics.**~~ **Confirmed as intentional safety deviation:** Disassembly of `0x14030f6d0` / `0x14030f7b0` shows retail only checks byte 0 for `'-'` and accumulates `(ch - '0') * pow10` mod `2^64` backward with zero validation; `RglString::parse_{i64,u64}` keeps strict full-slice validation (`wse/core/rgl_string.h`).
4. ~~**Simple-trigger early-out queue.**~~ **Resolved:** Confirmed `g_gameScreen` slot `0x12` is `mbMenuWindow`: `jump_to_menu` (`2060`, `0x1401dc2b1`) pushes `{0x12, -1}` to `world.m5.enqueued_windows` (`native_mission_ops.h`), `execute_simple_trigger_manager` (`0x1402101d2..0x1402101f7`) halts remaining triggers on positive odd keys when back is `0x12`, and `WseWorldState::consume_campaign_queued_window` (`mbMapWindow::frameMove` `0x140117f90`, pop at `0x140118987`; `mbMenuWindow::open` `0x140120240`) pops/clears the queue on campaign frames (`step_world_frame`, `headless_runner.h`, `block_vm_test.cpp`). Pop order is still open (Phase 2E review, below).
5. ~~**Hero flags while loading parties.**~~ **Verified:** Scanning `mbStatement::execute` (`0x1401c6150`) and `troop_set_type` (`0x1401d01f0`) confirms no runtime opcode toggles `tf_hero` (`0x10`) at `+0x128`; module troop hero flags remain invariant across `loadParties` (`0x1400c92c0`).
6. ~~**Un-renamed troops keep the live name.**~~ **Verified:** `mbLoadingWindow::doStep` (`0x140112630`, `0x140112890..0x140112978`) calls `mbGame::readSettingsData` (`troops.txt`) before `mbGame::readStream`, resetting module names cleanly prior to `loadTroops` (`0x1400c9650`).
7. ~~**Windows host gate not re-run.**~~ **Verified:** Full Windows `engine/build_host.ps1` (`21/21` suites, `selftest_host` `6,402 ms`, zero warnings) + retail `wse_headless.exe` & `wse_brf.exe` PASS (`§7`).

### Phase 2E review (`e6c4d0e`): resolved (all 10 items verified against `v1.1.5.5` and fixed)

Found in the review of `e6c4d0e` (PR #22 / `afab774`); all 10 follow-up items are now verified against `v1.1.5.5` and resolved:

1. ~~**Window-queue pop order.**~~ **Verified & Fixed (`0x140118987..0x140118a80`, `0x1400ef710`, `0x1400f0c50`):** `mbMapWindow::frameMove` peeks `enqueued_windows.back()` and ONLY pops/opens `0x12` (`mbMenuWindow` 0x12, clearing remaining entries in `mbMenuWindow::open` `0x140120240`), `0x0f` (`0x16` presentation, enqueued by `900 start_presentation` `0x1401cbf02`), and `0x10` (`0x15` notes, `2053` `0x1401dc108`), leaving all other types unpopped while syncing `g_gameScreen->m_windowStack` (`m5.active_window` and `m6.ui.active_window_id`).
2. ~~**`mbGame::removeParty` on the main party.**~~ **Verified (`0x1400dd990`, `0x1400de230`):** `removeParty` does not block party 0 (matching the SDK corruption warning; our `remove_party` keeps `party_no == main_party_no -> false` as an explicit safety guard), and `addPartyToParty` (`0x1400de230`) tail-calls `0x1400dd990` at `0x1400de480`, confirming `num_destroyed` increment in `party_join_to_party`.
3. ~~**`multiplayer_clear_scene` (`416`, `0x140147a80`).**~~ **Verified & Fixed (`0x140147e9f`, `0x140148098`, `0x14013a450`, `0x140153d80`):** `mbMission::clearScene` removes ONLY mission objects with `+0x1d5 != 0` (`m5.enable_pruning`, leaving scene-placed props and `1974 spawn_scene_prop` / `1971(..., 0)` objects intact without resetting them), and resets active (`status == 2`) players' `m_agentNo = -1` (`+0x1f4`) and `ready_to_spawn = 0` (`+0x1f8`) while preserving `item_ids`.
4. ~~**Mission-object fade-out completion.**~~ **Verified (`0x1400c6440`):** `mbFadingOutEntity::frameMove` calls `rglEntity::setAlpha` (`0x140294720`) on completion without changing `m_visible`, confirming leaving `visible` untouched is retail-accurate.
5. ~~**`rglHashVector<mbParty>` header.**~~ **Fixed (`0x1400e7de0`, `0x1400e7b80`):** Restored `WseWorldState::next_party_key` (`m_nextKey` `+0x138f0`); `saveParties` writes `std::max(parties.size(), next_party_key)` and `loadParties` restores `next_party_key` so `allocate_party` resumes past trailing tombstones.
6. ~~**Version-invariance of remaining readers.**~~ **Verified (`0x1400e7b80`, `0x1400e7640`, `0x1400bae70`):** Confirmed `rglHashVector<mbParty>::readStream`, `rglStableVector<mbEvent>::readStream`, and `mbEvent::readStream` contain zero `[802..1174]` version branches (`0x1400e7b80` only forwards `version` to `mbParty::readStream`), recorded in `docs/ARCHITECTURE.md` §8.
7. ~~**Combat options in `<= 1136` saves.**~~ **Verified (`0x1400c856d`, `0x140112630`):** Confirmed `mbGame::readStream` skips the block without resetting `rglConfig`, and `mbLoadingWindow::doStep` does not reload `rglConfig`, so keeping live options is retail-accurate.
8. ~~**Per-note `available` & date field.**~~ **Verified & Fixed (`0x1401b9e90`, `0x1401b9f30`, `0x1401cd7da..0x1401ce174`, `0x1401de2ee`):** `mbNotes` only stores the scalar object-level `m_available` (`+0x4c4`); removed dead per-note `WseQuestNote::available`, and fixed `1113 setup_quest_note` / `1114`/`1116`/`1118`/`1119` (`native_world_ops.h`, `native_campaign_ops.h`) to write `world.current_day` (matching `[mbGame + 0x13fd0]` proven by `2272 store_current_day`) instead of `world.game_year`.
9. ~~**Windows host gate for `e6c4d0e` and fixes.**~~ **Resolved:** Re-executed full Windows `engine/build_host.ps1` + retail `Native`/`Phantasy Calradia` validation after all fixes (§1, §2, §7).
10. ~~**References in `fx`.**~~ **Verified & Fixed (`world_state.h`):** `unlink_agent_references(-1)` clears `fx.trigger_agent_no = -1` on full mission clear; `fx.missiles` (append-only projectile launch log) and `fx.ray_obstacles` (static ray-cast geometry) carry no recycled agent slots and remain intact.

### Phase 2A / P9 Part 1 review (`5ab81a4`): fixed, with open items

Found in the review of `5ab81a4`; fixed on `claude/busy-sagan-3dohz1` (`engine/README.md` holds the
handler notes and safety deviations):

1. ~~**Stale `m6.ui.active_window_id`.**~~ **Fixed:** a consumed menu, notes, or presentation
   window was never replaced, so `3039`, `3044`, `4901`, and `4907` stayed blocked for the rest of
   the session. Window types the map does not open now leave the active window unknown (`-1`), and
   mission frames retire campaign window ids.
2. ~~**Window queue never drained.**~~ **Fixed:** every type other than `0x12`/`0x0f`/`0x10`
   (`change_screen_return`, `change_screen_map`, inventory/party screens, ...) accumulated forever.
   The campaign consumer pops them (documented deviation), and `enqueue_window` caps the queue at
   `kMaxQueuedWindows`.
3. ~~**`start_presentation` (900) in missions.**~~ **Fixed:** the `0x0f` entry is queued only on the
   campaign map, so missions no longer leak entries that reopen stale presentations on the map.
4. ~~**`m_nextKey` allocation.**~~ **Fixed:** a save or `parties.txt` with `m_nextKey` up to `2^20`
   made the first `allocate_party` allocate ~1.25 GB of tombstones. Party keys are capped at
   `kMaxPartyKeys` (`131072`) in both loaders and in `allocate_party` (callers handle `-1`).
5. ~~**Parties with `key >= m_numSlots` dropped on load.**~~ **Fixed:** the reader grows the
   key-indexed vector to the stored key; a key at or above `m_nextKey` is `corrupt_record`.
6. ~~**`instantiate_scene_objects`.**~~ **Fixed:** it now goes through `clear_mission_entities`
   (no stale selections / attached missiles / spawn effectors), keeps the `.sco` kind in
   `meta_mesh_no`, gives entry points and flora no `scene_prop_no`, fills `world.entry_points`,
   and returns the number of records dropped at `kMaxSpawnedMissionObjects`.
7. ~~**`scene_prop_set_prune_time` (1819).**~~ **Fixed:** a positive time arms `enable_pruning`, as
   the `+0x1d5` note (`setPruneTime` `0x140153d88`) states, so `416` and frame pruning remove it.
8. ~~**Skinning weights.**~~ **Fixed (documented deviation):** the 4 largest influences are kept and
   renormalized. Leftover debug lines in `parse_skeleton_bodies_xml`, the `.sco` 68-byte record
   bound, the dead `ai_face_count`, and `map.txt` count plausibility checks were also fixed.

Still open:

1. **Notes / presentation windows never close (partly fixed).** `UiSession::close_active_modal_window`
   (`wse/game/ui_runtime.h`) closes `0x15`/`0x16` correctly, and `run_headless_session` /
   `wse_headless` now drive a `UiSession` every frame (`UiSession::drive_frame`), so headless runs
   open, step, and close them. The Android shell still does not. See the P9 Part 2 review list
   below, item 1.
2. **Dense party storage.** `kMaxPartyKeys` bounds the tombstone cost (~160 MB worst case) but does
   not remove it; a sparse key-to-slot map would match `rglHashVector` exactly.
3. ~~**Root motion.**~~ **Checked (analytical):** `scene_test.cpp` verifies the root translation key against the rest origin with an analytical world pose. Not compared against a retail pose.
4. ~~**Skinned mesh updates.**~~ **Fixed:** `BrfSceneRenderer` caches rest vertices and skin weights per slot, skins into a scratch buffer, and keeps the index buffer; `scene_test.cpp` asserts IB reuse and pixel equivalence.
5. ~~**`Phantasy Calradia` does not load.**~~ **Fixed:** `item_kinds1.txt` record 910
   `itm_drow_c_dagger` stores its properties as a 43-digit decimal (a Python integer wider than 64
   bits). Retail `readUInt64` (`0x1402afbe0`) converts with `rglString::toUInt64Stream`
   (`0x14030f7b0`), an unchecked 64-bit digit sum, so the value wraps modulo 2^64 (`0x410002`);
   the port's `TokenReader::read_u64` rejected it. It now wraps all-digit tokens the same way
   (non-digit tokens are still rejected). `wse_headless` loads Phantasy Calradia (1,544 items,
   1,935 troops) and runs 200 frames with 0 errors and 0 unimplemented opcodes.

### P9 Part 2 review (`afe751c`): fixed, with open items

Fixed in the follow-up (regression tests in `ui_test.cpp`, behaviour notes in `engine/README.md`):
presentation trigger parameters now go through `world.misc.trigger_params` instead of overwriting
`reg0`/`reg1`; `presentation_set_duration` (902) drives the close countdown; overlay colours use
`overlay_set_alpha` as the alpha byte instead of multiplying a zero alpha; text buttons
(912/940/941) and image buttons (type 4) are the clickable overlays, empty text overlays no longer
draw white panels, and flags `0` stay left-aligned; `start_presentation` clears the previous
overlays and follows a `start_presentation` issued by a trigger; closing drops the modal widget tree,
is a no-op without a modal window, and only drops queued entries for the closing presentation;
Escape honours `prsntf_manual_end_only` and reports `PresentationClosed` (both withdrawn after the `a34ccbb` review: retail never closes a presentation on Escape, see item 3 below); `prev_window` defaults to
the current window; menus run their operations and format `{sN}`/`{regN}` text;
`measure_text` is alignment-independent and `font_scale <= 0` is rejected; the UI asset loader no
longer copies, decodes, or uploads non-UI meshes and lets module meshes override the fallback packs;
`material_name` resolves through materials; a failed font re-upload no longer leaves a destroyed
handle; `skin_brf_mesh` skips the discarded position transforms in its normal pass;
`update_skinned_mesh` rebuilds its cache for a different source mesh; the two vacuous `-62`/`-63`
assertions now use sentinels.

Still open:

1. **`UiSession` is driven headless only (partly fixed).** `UiSession::drive_frame` runs after
   `step_world_frame` in `run_headless_module` (`HeadlessRunOptions::drive_ui`, `wse_headless
   --no-ui` to turn it off): a queued `0x0f` starts the presentation (`-60`), a queued `0x10`
   opens the notes window, open presentations step (`-61`, duration countdown, auto-close), and a
   mission frame drops an open modal window. Still missing: the Android shell / SDL frame loop
   (pointer and key input through `handle_pointer_and_keys`, rendering with `UiRhiRenderer`),
   menus (fixed, see below), and mission-side presentations
   (now fixed, see below). The notes window text
   (object name, then description and note slots) is a port approximation of `mbNotesWindow`.
   The headless runner also clears `lighting.has_scene` after loading (option
   `start_on_campaign_map`): the struct default `true` made every headless frame a mission frame,
   so the campaign window queue was never consumed and `start_presentation` never queued.
   **Menus (fixed):** `UiSession::drive_frame` takes the module menus (new last parameter) and
   builds the menu window when `consume_campaign_queued_window` opens `0x12`, for
   `world.jump_menu_no` (set by `jump_to_menu` 2060, `0x1401dc2b1`) via `build_menu_window`, which
   runs the menu's operations (`mbMenuWindow::open` `0x140120240`). It closes an open
   notes/presentation window first. The port counter `world.m6.ui.menu_open_serial` (bumped on
   each consumed `0x12`) makes a menu-to-menu jump rebuild. The menu number from `jump_to_menu`
   and setting `world.ui.current_menu_no` are port choices. `run_headless_module` passes
   `bundle.menus`, and `wse_headless` prints `menus_opened=` (`HeadlessReport::menus_opened`).
   **App frame loop (built, device run pending):** `wse/platform/ui_game_shell.h` `UiGameShell` loads the UI assets and the module, opens on the main menu (`build_initial_window`), runs `game_start` on "Start a New Game" (`HeadlessModuleRunner::run_game_start`) and opens game menu 0 when `game_start` queues no window (port choice: retail rebuilds `mbGame` through the starting / loading windows first, not traced). Each display frame feeds SDL events through `UiInputAccumulator`, steps the module once (`HeadlessModuleRunner::step`, which hands the pointer to `handle_input` before `drive_frame`) and renders into an RGBA8 frame. `sdl_selftest_main.cpp` runs it with `--module <warband_root> [module_rel_dir]` (on Android the `wse.module_root` / `wse.module` intent extras through `WseActivity.getArguments`) and streams the frame into an SDL texture. Checked on the host against retail Native: the main menu draws, a click on "Start a New Game" opens `start_game_0`. **SAF installs (built, device run pending):** the launcher's "Run Selected Module (UI shell)" button starts `WseActivity` with `EXTRA_RUN_MODULE`; `WseActivity.onCreate` registers a `WseSafBridge` for the picked tree (`nativeRegisterGameTree`, a JNI global reference) and passes `--saf-module Modules/<name>`, and the SDL thread opens the tree with its own `JNIEnv` (`platform/android/saf_game_tree.h` `RegisteredGameTree`, indexed once before loading). **Frame cost (fixed):** `UiGameShell` hashes the widget tree (`UiTreeHasher`) and skips redrawing while it is unchanged, and the app now draws the UI on the GPU: `UiGameShellOptions::Output::geometry` records screen-space triangle batches (`wse/platform/rhi_geometry.h` `GeometryCaptureRhiDevice`) that `SdlGeometryPresenter` draws with `SDL_RenderGeometry` at the window's full pixel size, and the window is only presented when the frame changed. On retail Native at 2400x1080 with the pointer moving every frame, a redraw costs 0.033 ms of CPU (4 batches, 204 vertices) instead of 103 ms in the software rasterizer (capped at 720 lines). The debug APK's native code was built at `-O0` (Gradle passes `CMAKE_BUILD_TYPE=Debug`); `WSE_ANDROID_OPTIMIZE_DEBUG` (default on) compiles it at `-O2 -g`. GPU-side cost and the `SDL_RenderGeometry` output itself are unmeasured until a device run. **Still open:** no IME and no Lua runtime in the app; SAF reads go through JNI one file at a time (load time on a device unmeasured); nothing has run on a device.
   **Mission-side presentations (fixed, P9 #1):** `start_conversation_presentation` (4907, `0x1401f296e`) queues into `world.m6.ui.pending_conversation_presentations`, as `mbConversationWindow::startPresentation` (`0x1400888a0`) does. Each mission frame `UiSession::update_mission_presentations` runs `mbConversationWindow::updatePresentations` (`0x1400888c0`): a pending start ends every open container that shows the same presentation or lacks `prsntf_manual_end_only`, then opens and starts one (`-60`, `mbPresentationContainer::startPresentation` `0x1402097b0`); every container runs `-61` and closes when its duration runs out. `presentation_set_duration` (902, mission branch `0x1401cc016`) and `is_presentation_active` (903, mission branch `0x1401cc135`) reach these containers through `m6.ui.mission_duration_requests` and `m6.ui.active_presentations`. Leaving the scene clears the stack. Native 900 has no mission branch (`0x1401cbf02` only queues `0x0f`). Each container keeps its own overlay tree and interactive state (hover, armed button, drag, number-box hold, combo popup, scroll, text-box focus); while any container is open `UiSession::root` draws them in stack order (later on top). `handle_input` routes the pointer to them top container first: hover enter / leave reach every container, and a press one container consumes is not handed to those below (`rglContainerWidget::triggerState` `0x14028af30`). A container's triggers run with its own number; a `start_presentation` (900) inside one cannot renumber it.

2. **Overlay size semantics (mostly resolved).** Checked against the retail widgets: text size
   is `0.02 * size` (`rglWidget::setScale` `0x14031e7e0`), mesh overlays scale their mesh by the
   size (`rglImageWidget::setScale` `0x1402a6bd0`), a `create_button_overlay` box is its text
   extent at creation (`rglWidget::setText` `0x14031e330`, flag `0x200`; measured with
   `UiSession::font`), and game / in-game buttons have `0.148 x 0.04` / `0.2123 x 0.04` boxes
   that `overlay_set_size` replaces (`rglGameButtonWidget::setScale` `0x14029bfb0`). **912
   re-measure (fixed):** `overlay_set_text` (920) records the `overlay_set_size` scale in force
   (`m5.text_measure_scale`), since retail `setOverlayText` (`0x14020df90`) calls
   `rglWidget::setText` (`0x14031e330`), which re-measures the box at the current text size
   (0.02 x scale); `UiSession` measures 912 boxes at that scale. **Game-button meshes (fixed):**
   `createGameButtonOverlay` / `createIngameButtonOverlay` set the mesh scale as retail does, and
   `rglGameButtonWidget::setScale` rescales the meshes with the box (`docs/ARCHITECTURE.md` §8 item 18).
   Still approximate: the anchoring of area-less overlay text has not been compared with a retail
   screenshot. Hosts must set `UiSession::font` (nothing in
   `wse_headless` renders, so it stays null there).
3. ~~**`prsntf_manual_end_only` (`0x2`) has no retail VA.**~~ **Resolved:** the only retail test of
   presentation flags bit `0x2` (capstone sweeps of `.text` for direct and register-loaded bit tests
   on `mbPresentation+0x48` / `mbPresentationContainer+0x1c`) is in
   `mbConversationWindow::updatePresentations` (`0x1400888c0`): when a presentation queued by
   `mbConversationWindow::startPresentation` (`0x1400888a0`) starts, every other open container is
   ended unless it has this bit. `mbPresentationWindow::frameMove` (`0x140174ed0`) never reads it
   and has no Escape check, so `UiSession` no longer closes presentations on Escape (the earlier
   `PresentationClosed` behaviour had no retail basis; 17 of Native's 49 presentations handle
   Escape themselves through `key_clicked`). `SYMBOL_MAP.json` now curates `mbConversationWindow`
   (`build_symbol_map.py` token; regenerated from the v1.1.5.5 PDB, +10 symbols). Honouring the bit
   is applied by the mission-side presentation stack (item 1), `update_mission_presentations`.
4. ~~**Mesh overlays ignore colour and alpha.**~~ **Fixed (software path):** retail
   `rglImageWidget::setColor` / `setAlpha` (`0x1402a6c50` / `0x1402a6c90`) and
   `rglImageButtonWidget::setColor` (`0x1402a6a80`) write the overlay colour into the widget
   mesh's colour (`rglMesh+0xc0`, RGB and alpha bytes); game buttons keep the base
   `rglWidget::setColor` (`0x14031e890`) and do not tint their meshes. `MeshDrawCall::tint_rgba`
   now multiplies every vertex colour in the software rasterizer, and `UiWidget::tint_mesh` (set
   for mesh overlays and image buttons) makes `UiRhiRenderer` pass `color_argb` as the tint.
   The Vulkan device still draws through the software rasterizer (P4 item 1), so the tint must be
   added to the real GPU pipeline when that lands. **Overlay animation (fixed):** opcodes 932-937
   capture the current value as the start and start a channel (`native_ui_start_anim`), and
   `UiSession::step_presentation` interpolates colour, alpha, size and position per retail
   `rglWidget::frameMove` (`docs/ARCHITECTURE.md` §8 item 18).
5. ~~**Interactive overlays are incomplete.**~~ **Mostly fixed (P9 #5):** `UiSession::handle_input`
   (new `UiPointerInput`; `handle_pointer_and_keys` stays as a one-frame tap wrapper) follows
   retail `rglWindowManager::frameMove` (`0x14032a0d0`): left / right press -> widget states
   `0x10` / `0x40`, release -> `0x8` / `0x20`, sent to the focused widget, else the window tree.
   `rglContainerWidget::triggerState` (`0x14028af30`) gives hover enter `0x2` / leave `0x4` to
   every visible, enabled child the cursor enters or leaves (any type) and presses to children
   under the cursor top-down until one consumes it. `mbPresentationContainer::frameMove`
   (`0x140208ec0`) then raises, per overlay in index order, `-63` (enter 0 / leave 1), `-62`
   (state `0x10000` with the per-type value), and `-64` (`0x10` / `0x40` / `0x100` = param 2
   0 / 1 / 2). Value widgets: plain and toggle buttons (`rglSimpleButtonWidget::triggerState`
   `0x140307770`; check box = `createCheckBoxOverlay` `0x14020bc50`), sliders (`createSliderOverlay`
   `0x14020af30`, `rglSliderWidget::triggerState` `0x14030d610`), number boxes
   (`rglNumberBoxWidget::triggerState` `0x1402c8310`, auto-repeat 1/3/10/50), combo labels
   (`rglComboLabelWidget::triggerState` `0x140262cd0`) and combo buttons
   (`rglComboButtonWidget::triggerState` `0x140261340`); the wheel scrolls the nearest
   `rglScrollableWidget` (`createTextOverlay` `0x140209a90`); container children are now offset,
   culled and scrolled (`rhi_ui.h`). Evidence: `docs/ARCHITECTURE.md` §8 item 17. The same
   change fixes `create_text_overlay` (910) ignoring `set_container_overlay`. **Still open:**
   number-box / combo-label arrows are the outer quarters of a fixed box (the popup's position
   and item height now follow `rglComboButtonWidget::openPopup` `0x140261d10`, §8 item 20), list
   boxes act like combo buttons, the slider grab offset is the cursor-to-knob distance, scroll step 0.05
   per notch with content-top start, no scissor (clipping culls children wholly outside), plain
   button `-62` param 2 passes the overlay value (retail's is a stale trigger parameter), triggers
   run at input time rather than after `-61`, text boxes now take typed characters (**fixed approximately**, `docs/ARCHITECTURE.md` §8 item 18:
   a left press focuses, Enter / Escape / an outside press commits with `-62` and the text, backspace
   removes one character; no IME composition (numeric number-box entry, `rglTextBoxWidget::setIntValue` `0x140316a10`, is fixed, and placeholders are unreachable from module overlays, §8 item 20; the 28-character cap, `rglTextBoxWidget` `0x140316900`, and the focus commit by `overlay_obtain_focus` (949) are fixed), number-box and combo meshes are not drawn (text only; sliders draw their track and knob, `createSliderOverlay` `0x14020af30`, `rglSliderWidget::getPercentage` `0x14030dae0`), the middle-press (`0x100`) sender is untraced, and
   area-less text overlays have no hover box (needs the item 2 anchoring check).
6. ~~**Per-frame GPU buffer churn.**~~ **Fixed:** `RhiDevice::update_buffer(handle, data, error_out)`
   (`rhi.h`) replaces a live buffer's contents. `SoftwareRhiDevice` writes it directly, and
   `VulkanRhiDevice` re-creates its mirrored VkBuffer only when the data grows. `MeshDrawCall::first_index`
   lets draws share one index buffer. `UiRhiRenderer::render_tree` (`rhi_ui.h`) keeps one dynamic
   vertex buffer and one index buffer across frames (refilled with `update_buffer`; each text or
   panel batch uses `first_index`). `BrfSceneRenderer::update_skinned_mesh` (`rhi_scene.h`) refills
   its vertex buffer, and its index buffer when the source mesh changes, in place. **Still open:**
   the Vulkan device still rasterizes in software (P4 item 1), so real GPU-side upload and sync
   semantics come with the real pipeline.
7. ~~**Retail UI check not re-run.**~~ **Done:** `ui_test.cpp`'s retail section ran on Windows
   against the GOG Warband install (`build_host.ps1`, `4c6bd95`) and passed with the filtered
   asset loader (Native main menu meshes, textures, font, language, and render checks).

### `a34ccbb` review (follow-up to the P9 Part 2 fixes): fixed

1. ~~**Self-restarting presentations closed.**~~ **Fixed:** `follow_presentation_state` only
   switched when `active_presentation_no` changed, so `presentation_set_duration 0` +
   `start_presentation <self>` (12 of Native's 49 presentations, e.g.
   `prsnt_game_custom_battle_designer`, `prsnt_multiplayer_item_select`, `prsnt_custom_banner`)
   closed window `0x16` and dropped the queued restart. `start_presentation` (900) now bumps the
   port counter `m6.ui.presentation_start_serial`, and `UiSession` restarts the open presentation
   when it changes (in or out of a scene); `ui_test.cpp` covers both.

## 6. Open documentation questions

~~One open question (2026-11-14 review): `mbStatementBlock::execute` (`0x1401f69d0`) local-variable zeroing condition.~~ **Resolved:** At `0x1401f6c59` (inside `mbStatementBlock::execute` `0x1401f69d0`), the VM checks `rglConfig::General::bLocalVariablesZeroInitialization` (`0x140617819`, loaded from `[General] bLocalVariablesZeroInitialization` in `rglConfig::load` at `0x14026f716` via `rglIni::getBool` `0x1402a84d0`), whose `.data` default is `1` (`true`). `VmContext::clearLocalsBetweenCalls` now defaults to `true` (`docs/ARCHITECTURE.md` §5 Block execution).

Previously closed: the four 2026-11-13 questions (`types_x64`/`globals_x64`/`vtables_x64` naming and SHA-256 hashes, 73/75/76 vtable tally, `docs/ARCHITECTURE.md` §2 v1.1.5.5 type counts, and automated `0x14…` citation checking) were resolved in B7 (`docs/ARCHITECTURE.md` §1–§3, §8–§9; `tools/check_citations.py`).

## 7. Evidence & analysis ledger

| Check | Result | Evidence |
| --- | --- | --- |
| Target extraction | PASS | v1.1.5.5 `mb_warband_wse2_x64.exe` 7,060,992 B, SHA-256 `d4e97d56…4e29709`; official `WSE2.zip` `c45197e8…276e28b1` |
| REA/Ghidra analysis | PASS | v1.1.5.5 analysis (`ev_e323351d…` / `ev_fa7fc7d6…`): 9 segments, 20,276 procedures, 7,448 strings; image base `0x140000000`; PDB `fccf5e8d…8cfc` age 449 |
| PE import inventory | PASS (v1.1.5.5) | `ev_a4af0bca5d107b626454d91f4c69c2aef5d4f21b466cbb174333d7c8cd687fc6`: 23 DLLs (21 static + 2 delay), 482 + 64 = 546 functions |
| PDB parse (MSF 7.0/TPI/DBI) | PASS | 156,846 TPI records, 15,363 sized layouts |
| PDB symbol map | PASS | 48,658 symbols with VAs (`globals_new.json`) |
| Vtable scan | PASS | `tools/vtables.py`: 279 pointer runs `>= 4`; 207 class-identified; 73 `rgl*`/`mb*`/`WSE*` (byte-identical to `vtables_new.json`) |
| Citation check (B7) | PASS | `tools/check_citations.py --self-test --docs` (`1,142` doc VA references across 6 living docs checked against `SYMBOL_MAP.json`, with `tools/docs_citation_baseline.txt` holding `36` pre-existing uncurated image-base/`.data`/historical VAs) validates every `0x14…` address and rejects superseded `ev_*` / synthetic addresses across all `83` headers (`2,260` VA refs / `1,807` unique VAs) under `engine/include/` (validated against committed maps and `v1155/` PDB/globals/PE ranges) |
| Tracking documents | PASS | `AGENTS.md`, `ROADMAP.md`, `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (83 classes, 3,687 symbols, 76 vtables, 5,046 function ranges, 25 global objects, 3,052 curated superseded VAs), v1.1.5.5 target |
| v1.1.5.5 REA VM/stream batches | PASS | 75 procedures across 9 `decompile_all` exports (`stmt_core` `ev_3ebcea85…` 4/4, `stmt_block` `ev_ffc38aa5…` 10/10, `script` `ev_8d37a0c8…` 4/4, `simple_trigger` `ev_3157906d…` 12/12, `trigger` `ev_f62843ac…` 12/12, `game_scripts` `ev_fc9bd1a4…` 4/4, `game_triggers` `ev_052f2d86…` 2/2, `streams_bin` `ev_8cc48986…` 9/9, `streams_txt` `ev_1e3c2566…` 18/18), plus `read_bytes`/`0x140615ad0` (`ev_f09662c6…`/`ev_2ddf7a62…`) and 6 single-procedure exports |
| Superseded 2026-10-14 REA runs | PASS (superseded) | Historical 2026-10-14 entries kept for provenance only: `ev_7cd99ccc…157d7a` (`mbScript` 5), `ev_8d91b175…99b11` (`mbStatement` 14), `ev_25469ff4…84af8` (3), `ev_e3530f99…e8d05` (`Trigger` 52), `ev_cebc7ae0…e8c8d3` (`rglTextStream` 18), `ev_24eecfa4…f18c0b6` (`rglBinaryStream` 9), `ev_130d8023…2c62e6` (2) |
| Slice P9 Part 2 UI runtime, bitmap fonts & `Native` main menu | PASS (Windows `build_host.ps1` `23/23` / `180` funcs, `selftest_host` `23/23` `18,328 ms` incl. `ui` `1,862 ms` with retail `Native` UI at `WSE_WARBAND_ROOT`; all `83` headers compile standalone with `-Wall -Wextra -Werror`; `check_citations.py --self-test --docs` `83` headers / `2260` VA refs; `extract_wse_opcodes.py --check` `440/440`) | `wse/io/xml.h`, `wse/asset/font.h`, `wse/game/ui_runtime.h`, `wse/platform/rhi_ui.h`, `ui_test.cpp`; closed P9 Part 1 review items #1/#3/#4. Linux/CMake/CTest, ASan/UBSan, and CI were not run for this change. |
| `afe751c` review fixes (P9 Part 2 UI runtime) | PASS on Linux (Ubuntu `clang++` 18.1.3): header-only gate loop `engine host tests: PASS` (19 suites incl. `ui` + `script`); CMake/CTest `24/24` incl. `platform`, `lua`, `asset`, `audio`, `ui`, `selftest_host`; `wse_headless` + `wse_brf` built; `g++` ASan/UBSan `ui`, `scene`, `native_m5` PASS; changed headers compile standalone with `-Wall -Wextra -Werror`; `check_citations.py --self-test --docs` (`83` headers / `2260` VA refs, `1189` doc refs); `extract_wse_opcodes.py --check` `440/440` | `ui_runtime.h`, `rhi_ui.h`, `font.h`, `xml.h`, `skeleton.h`, `rhi_scene.h`, `world_m5_types.h`, `native_ui_ops.h`, `ui_test.cpp`. Retail `WSE_WARBAND_ROOT` checks, Windows `build_host.ps1`, and the Android NDK gate were not run here (CI runs NDK on push). |
| `a34ccbb` review + headless `UiSession` driver | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` incl. retail `ui`; `check_citations.py --self-test --docs` 0 unresolved; `wse_headless --frames 200 --allow-unimplemented`: Native, FiveDynastiesAndTenKingdoms, Suvarnabhumi Mahayuth 1_19 each `presentations_opened=1`, `ok=true`, 0 errors, 0 unimplemented; Phantasy Calradia `LOAD FAILED` before and after). Linux/ASan via CI only | `ui_test.cpp` (self-restart, `drive_frame`), `headless_test.cpp` (`test_headless_drives_presentation_window`) |
| `read_u64` modulo-2^64 wrap (Phantasy Calradia) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`; `module_loaders` u64 cases; `wse_headless --frames 200 --allow-unimplemented` on Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms, Suvarnabhumi Mahayuth 1_19: all `ok=true`, 0 errors, 0 unimplemented). Linux/ASan via CI only | `0x1402afbe0` / `0x14030f7b0` capstone disassembly of the v1.1.5.5 PE |
| `prsntf_manual_end_only` trace + Escape change | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`; `check_citations.py --self-test --docs`; `SYMBOL_MAP.json` regeneration reproduces the committed map byte-for-byte before the `mbConversationWindow` token) | capstone `.text` sweeps (direct + register-loaded bit tests at `+0x48` / `+0x1c`, validated on `startPresentation`'s bit-0 read); `0x1400888c0`, `0x1400888a0`, `0x140174ed0`, `0x1402097b0` |
| Overlay button boxes / size semantics | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`; `check_citations.py --self-test --docs`; `ui_test.cpp` `test_overlay_button_boxes`) | capstone disassembly of `0x14020e0c0`, `0x14031e7e0`, `0x1402a6bd0`, `0x14020abe0`, `0x14031e330`, `0x14031dfa0`, `0x14020c3e0`, `0x14020c7d0`, `0x14029bfb0` |
| Mesh overlay tint | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`; `check_citations.py --self-test --docs`; `ui_test.cpp` tint render checks) | capstone disassembly of `0x14020e040`, `0x1402a6c50`, `0x1402a6c90`, `0x1402a6a80`, `0x14031e890` |
| Interactive overlays (P9 #5) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `29,907 ms` incl. retail `ui`; changed headers compile standalone with `-Wall -Wextra -Werror`; `check_citations.py --self-test --docs` `83` headers / `2312` VA refs, `1308` doc refs) | `ui_runtime.h` `handle_input` / `UiPointerInput`, `rhi_ui.h` container offset + culling, `native_ui_ops.h` / `native_entity_ops.h` `create_opcode` + 910 container fix, `ui_test.cpp` (3 new functions); `SYMBOL_MAP.json` +8 class tokens (3,835 symbols). Evidence: `docs/ARCHITECTURE.md` §8 item 17. Linux/ASan/NDK left to CI. |
| GPU buffer reuse, 912 re-measure, `0x12` menus, overlay animation, text boxes, game-button meshes (P9 #1/#2/#4/#5/#6) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `18,906 ms` incl. retail `ui`; changed headers compile standalone with `-Wall -Wextra -Werror`; `check_citations.py --self-test --docs` `83` headers / `2333` VA refs, `1344` doc refs) | `rhi.h` `update_buffer` / `first_index`, `driver_loader.{h,cpp}`, `rhi_ui.h`, `rhi_scene.h`, `ui_runtime.h`, `native_ui_ops.h`, `native_entity_ops.h`, `world_m5_types.h`, `world_m6_types.h`, `world_state.h`, `headless_runner.h`, `wse_headless.cpp`; `platform_test` / `ui_test` / `scene_test`; `SYMBOL_MAP.json` +`rglTextBoxWidget` (3,844 symbols). Linux/ASan/NDK left to CI. |
| Mission presentations, `ui_frame.h`, SAF rename, headless audio (P9 #1/#5, P4 #6, P6 #1/#2) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `9,396 ms` incl. retail `ui`; changed headers compile standalone with `-Wall -Wextra -Werror`; `check_citations.py --self-test --docs` `84` headers / `2350` VA refs, `1376` doc refs; `wse_headless --frames 200 --allow-unimplemented --audio` on Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms, Suvarnabhumi Mahayuth 1_19: `ok=true`, 0 errors, 0 unimplemented, 19,200,000 frames mixed each, 357 / 1,007 / 469 / 822 samples resolved) | `ui_runtime.h` mission stack, text-box cap, 949 commit, slider meshes; `ui_frame.h` (new); `saf_vfs.h`; `headless_runner.h` / `wse_headless.cpp` audio; `native_entity_ops.h` 902/903 mission branches; `ops/ui_menu.h` 4907; `world_m5_types.h` / `world_m6_types.h`; `ui_test` / `platform_test` / `headless_test`; `SYMBOL_MAP.json` +`??0rglTextBoxWidget` (3,845 symbols). Android `saf_vfs_jni.cpp` / `WseSafBridge.kt` compile in CI only. |
| Mission presentations drawn and interactive (P9 #1) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `7,564 ms` incl. retail `ui`; `check_citations.py --self-test --docs` `84` headers / `2352` VA refs, `1379` doc refs; `wse_headless --frames 200 --allow-unimplemented` on Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms, Suvarnabhumi Mahayuth 1_19: `ok=true`) | `ui_runtime.h` per-container trees and input state, `handle_mission_input`, `compose_mission_root`; `ui_test.cpp` `test_mission_presentation_draw_and_input` |
| P10.1-P10.8: launcher pages, look, scan and install check, settings; async start, performance hub, texture budget, performance hints | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `8,759 ms`; `check_citations.py --self-test --docs` `87` headers / `2369` VA refs, `1408` doc refs; each unit re-verified on its own worktree before merging, P10.5 three times for races). Kotlin, SDL and JNI compile in CI only; device run pending | `headless_test` `test_launcher_install_check_json`, `test_list_warband_languages`, `test_ui_game_shell_start_async`; `platform_test` `test_p10_launcher_settings`; `ui_test` `test_texture_budget`; `docs/PLATFORM_PLAN.md` P10 status |
| P10.0: landscape module mode, missing-file dialog; P10 plan | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `10,007 ms`; `check_citations.py --self-test --docs` `87` headers / `2369` VA refs, `1408` doc refs; `find_missing_warband_files` on the retail install: Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms and Suvarnabhumi Mahayuth 1_19 complete, a `Modules/` root reports the missing module folder). First device run (user, APK `64250bd`): module UI works. SDL and Kotlin changes compile in CI only | `ui_game_shell.h` `find_missing_warband_files`, `format_missing_warband_files`, `check_install`, `missing_files()`; `sdl_selftest_main.cpp` landscape hint, `SDL_ShowSimpleMessageBox`; `WseActivity` sensor-landscape; `WseLauncherActivity` `AlertDialog`; `headless_test` `test_missing_warband_files`; `docs/PLATFORM_PLAN.md` P10 |
| GPU UI drawing in the app (`GeometryCaptureRhiDevice` + `SDL_RenderGeometry`), `-O2` debug APK | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `14,328 ms`; `check_citations.py --self-test --docs` `87` headers / `2369` VA refs, `1408` doc refs; `ui_test` `test_geometry_capture_matches_software`: captured batches replayed through `SoftwareRhiDevice` match the direct render within 0.5% of pixels; host probe on retail Native at 2400x1080, pointer moving every frame: 0.033 ms per redraw with geometry output vs 103 ms software). `sdl_selftest_main.cpp` (`SdlGeometryPresenter`) and the `CMakeLists.txt` Android flag compile in CI only | `rhi_geometry.h` (new); `ui_game_shell.h` `Output::geometry`, `geometry()`, `geometry_device()`; `sdl_selftest_main.cpp` presenter and present-on-change; `CMakeLists.txt` `WSE_ANDROID_OPTIMIZE_DEBUG`; `ui_test`, `headless_test` |
| App shell frame reuse and SAF-picked installs (P9 Part 2 item 1) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `9,081 ms`; `check_citations.py --self-test --docs` `86` headers / `2369` VA refs, `1408` doc refs; `saf_vfs_jni.cpp` compiled with `-Wall -Wextra -Werror` against JDK 17 `jni.h` and a stub `<android/api-level.h>`; host probe on retail Native: 120 frames averaged 11.5 ms, was 106 ms). `sdl_selftest_main.cpp` and the Kotlin changes compile in CI only; `Engine CI` passed 4/4 on `1ef86b6` | `ui_game_shell.h` `UiTreeHasher`, `frame_serial`, `frames_reused`; `saf_game_tree.h` (new) + `saf_vfs_jni.cpp` `nativeRegisterGameTree` / `RegisteredGameTree`; `sdl_selftest_main.cpp` `--saf-module`, texture re-upload on change and after render resets; `WseActivity` / `WseLauncherActivity` / `WseSafBridge`; `headless_test` `test_ui_game_shell` (reuse, hover redraw, tree hash) |
| App frame loop: `UiGameShell`, stepped `HeadlessModuleRunner` (P9 Part 2 item 1) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `8,672 ms`; `check_citations.py --self-test --docs` `86` headers / `2369` VA refs, `1408` doc refs; `wse_headless --frames 200 --allow-unimplemented` on Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms, Suvarnabhumi Mahayuth 1_19: `ok=true`, 0 errors, 0 unimplemented, 10.4 / 35.9 / 40.1 / 35.5 s; host probe of `UiGameShell` on retail Native: main menu rendered, "Start a New Game" ran `game_start` and opened `start_game_0`, 0 errors, 0 unimplemented, about 106 ms per 1280x720 software frame). `sdl_selftest_main.cpp` and `WseActivity.kt` compile in CI only | `headless_runner.h` `HeadlessModuleRunner` (`start` / `run_game_start` / `step`; `run_headless_module` loops it); `ui_game_shell.h` (new); `sdl_selftest_main.cpp` `run_module_shell`; `WseActivity.getArguments`; `headless_test` `test_headless_module_runner_steps`, `test_ui_game_shell` |
| Nested script frames, skill modifier in `store_skill_level`, interpreter profile | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `8,056 ms`; `check_citations.py --self-test --docs` `85` headers / `2367` VA refs, `1408` doc refs; `wse_headless --frames 200 --allow-unimplemented` on Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms, Suvarnabhumi Mahayuth 1_19: `ok=true`, 0 errors, 0 unimplemented, 11.1 / 33.2 / 35.9 / 32.7 s; microbenchmark 35–61 ns per statement) | `block_vm.h` `VmContext::activeBlocks`, nested `execute_script` at `currentDepth + 1`, frame vectors reserved up front; handler 2170 uses `troop_skill_level`; `block_vm_test` `test_party_limits_and_skill_modifier` (nested-frame check). Evidence: `docs/ARCHITECTURE.md` §8 item 21. |
| Placeholder audit: party limits and skill modifier, four handlers confirmed | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `8,231 ms`; `check_citations.py --self-test --docs` `85` headers / `2367` VA refs, `1407` doc refs; `wse_headless --frames 200 --allow-unimplemented` on Native, Phantasy Calradia, FiveDynastiesAndTenKingdoms, Suvarnabhumi Mahayuth 1_19: `ok=true`, 0 errors, 0 unimplemented, 9.7 / 34.7 / 38.2 / 34.0 s) | `block_vm.h` `troop_skill_level`, `party_max_companions`, `party_max_prisoners`; `world_campaign_helpers.h` retail fallback; handlers 101-107, 1633, 1634; `block_vm_test` `test_party_limits_and_skill_modifier`. 2170 fixed in the next row. Evidence: `docs/ARCHITECTURE.md` §8 item 21. |
| Number-box entry and range, combo popup height, shader warm-up (P9 #5, P5 lowering #11) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `8,786 ms` incl. retail `ui`; `shader.h` compiles standalone with `-Wall -Wextra -Werror`; `check_citations.py --self-test --docs` `85` headers / `2359` VA refs, `1391` doc refs) | `ui_runtime.h` `set_number_box_value`, numeric typing, popup item height; `shader.h` `SpirvShaderCache::warm_up`; `ui_test` number-box cases; `asset_test` warm-up cases. Evidence: `docs/ARCHITECTURE.md` §8 item 20. |
| One settings source, mixer worker thread, SDL audio lifecycle (P8 #4, P6 #1/#2) | PASS (Windows `build_host.ps1` with `WSE_WARBAND_ROOT`: `engine host tests: PASS`, `selftest_host` `23/23` `8,286 ms`; `audio_worker.h` / `performance.h` compile standalone with `-Wall -Wextra -Werror`; `check_citations.py --self-test --docs` `85` headers / `2352` VA refs, `1379` doc refs) | `performance.h` launcher fields + `save_launcher_choices`; `audio_worker.h` (new); `platform_test` settings merge cases; `audio_test` `test_audio_mixer_worker_thread`. `sdl_selftest_main.cpp`, `saf_vfs_jni.cpp` and the Kotlin launcher / activity / bridge compile in CI only. |
| Review fixes for `5ab81a4` (Phase 2A / P9 Part 1) | PASS (Linux clang++ 18.1.3: header-only gate loop `18/18` + `script`, CMake + CTest `23/23` incl. `selftest_host` (glslang from a local `16.6.0` checkout); g++ 13 ASan/UBSan on the touched suites `savegame`, `block_vm`, `native_m5`, `native_world`, `module_loaders`, `headless`, `scene`; `check_citations.py --self-test --docs`). Full `WSE_SANITIZE` CMake tree not run locally (disk allowance); CI `host-sanitizers` covers it | Fixed the 8 items in §5 "Phase 2A / P9 Part 1 review (`5ab81a4`)": stale `m6.ui.active_window_id`, undrained window types + `kMaxQueuedWindows`, `900` queueing in missions, `kMaxPartyKeys` bound on `m_nextKey`, sparse party keys past `m_numSlots`, `instantiate_scene_objects` references / kinds / entry points, `1819` pruning flag, top-4 renormalized skin weights; plus the `.sco` / `map.txt` bounds and leftover parser lines. Regression tests in `block_vm_test`, `native_m5_test`, `savegame_test`, and `scene_test` |
| Phase 2E review resolutions (`afab774` PR #22) + **Phase 2A completion & Slice P9 Part 1** | PASS (Windows `build_host.ps1` `22/22` / `174` funcs, `selftest_host` `6,495 ms` + retail `Native`/`Phantasy Calradia` CLI + `check_citations` + `extract_wse_opcodes`) | Resolved ALL `10` open §5 items from `afab774`: `consume_campaign_queued_window` `0x12`/`0x0f`/`0x10` filter + `m6.ui.active_window_id` sync (`0x140118987`/`0x1400f0c50`), `900` `start_presentation` queue (`0x1401cbf02`), `2055` loading state, `mbGame::removeParty` / `addPartyToParty` (`0x1400dd990`/`0x1400de230`), `416` `m5.enable_pruning` filter & active-player reset (`0x140147a80`), `rglHashVector<mbParty>` `m_nextKey` (`+0x138f0`, `0x1400e7b80`/`0x1400e7de0`), `[802..1174]` version-invariance (`0x1400e7640`/`0x1400bae70`), dead `WseQuestNote::available` removal (`0x1401b9e90`) + `current_day` fix in `1113`/`1114`/`1116`/`1118`/`1119` (`0x1401de2ee`), and `fx.trigger_agent_no` reset. Completed **Phase 2A** (`wse/game/scene_loaders.h`: `map.txt` `0x1400d9a00`, `SceneObj/*.sco` `0x140210be0`, `Data/skeleton_bodies.xml` `0x1400cbb70`/`0x140211ba0`, `dialog_states.txt` confirmed absent from retail binary) & **Slice P9 Part 1** (`wse/asset/brf.h` `skeleton_anim` `(qx,qy,qz,qw)` fix `0x14030c320`, `wse/asset/skeleton.h` FK & CPU linear-blend skinning `0x14030a770`/`0x14030b8f0`/`0x140258800`, `wse/platform/rhi_scene.h` terrain chunking + `BrfSceneRenderer`, `scene_test.cpp`); regenerated `docs/SYMBOL_MAP.json`; validated retail `Native` `wse_headless` (`20` frames / `755` parties / `errorCount=0`), `wse_brf` (`Native` `221/221`, `Phantasy Calradia` `333/333`, `271/271` shaders at `spv_fail=0`), and all `1,382/1,382` `Native` + `Phantasy Calradia` `.sco` files |
| Phase 2E review fixes (`e6c4d0e`) | PASS (Linux clang++ 18: header-only gate loop, CMake + CTest; g++ ASan/UBSan touched suites; `check_citations.py --self-test --docs`) | Fixed in review of `e6c4d0e`. **Mission objects:** `scene_prop_fade_out` (1822) no longer deletes the prop (`scene_prop_fade_in` 1823 could not restore it and the id was recycled); `multiplayer_clear_scene` (416) keeps scene-placed props (`spawned_at_runtime`) and no longer wipes player equipment; `remove_mission_object` also drops `attached_missiles` / edit-mode selections. **Agents:** `unlink_agent_references` clears team leader / order listener / division leaders, look / AI targets, offers, cached enemies, one-sided horse/rider links, `parent_agent_no`, camera target, and dialog speaker before a slot is recycled. **Players:** `disconnect_player` keeps `mp.saved_ban`, so `ban_player_using_saved_ban_info` (468) works after a kick. **Parties:** `remove_party` clears `encountered_party_2_no` and refuses the main party; `attach_party` refuses cycles; dead `num_parties_created` removed. **Frame step:** `step_world_frame` consumes the window queue only on the campaign map. **Savegame:** `<= 1136` bodies keep live combat options; the mismatched-header test pins `truncated`; dead `legacy_layout_unsupported` check removed. **Misc:** free-list release is O(1); `kQueuedWindowSaveExitBreakType` folded into `kQueuedWindowMenuType`; entry VAs `0x140117f90` / `0x140120240` / `0x14002a220` cited. Open items: §5. |
| Phase 2E container lifetimes + Phase 2A/P8 follow-ups | PASS (Windows `build_host.ps1` `21/21` `6,402 ms` + retail `Native`/`Phantasy Calradia` + `check_citations` + `extract_wse_opcodes`) | Resolved all 7 open §5 Phase 2A items + P8 zoom bounds from `v1.1.5.5` and implemented **Phase 2E** live entity lifetimes: **`savegame.h` / `rgl_string.h`:** gated `[802..1174]` bodies across `mbGame::readStream` (`0x1400c856d`) and `mbParty::readStream` (`0x1401ba9d1`/`0x1401baa20`/`0x1401baa7f`), verified scalar `mbNotes` `+0x4c0`/`+0x4c4` (`0x1401b9e90`/`0x1401b9f30`), `loadParties` unchanged `tf_hero`, `mbLoadingWindow::doStep` (`0x140112630`) `readSettingsData` reset before `readStream`, and intentional strict `toInt64Stream` (`0x14030f6d0`); **`native_mission_ops.h` / `headless_runner.h` / `world_state.h`:** `jump_to_menu` (`2060`, `0x1401dc2b1`) pushes `{0x12, -1}` (`mbMenuWindow`) consumed by `consume_campaign_queued_window` (`0x140117f90`/`0x140120240`); **`world_subsystem_types.h` / `world_input.h`:** `5.0f` default (`0x140116d70`) & `[3.5f, 175.0f]` zoom bounds (`0x1406173c0`/`0x1406173c4`); **Phase 2E (`world_state.h`, `world_campaign_helpers.h`, `module_*.h`, ops):** monotonic `allocate_party` + `mbGame::removeParty` (`0x1400dd990`) teardown + bidirectional `attach_party`/`detach_party`; shared `rglStableVector` LIFO free lists & tail shrink (`0x14014b960`/`0x14014b810`/`0x14014c030`/`0x14014bdc0`, `0x140138590`/`0x140132ba0`) for `agents`/`mission_objects` + 30s fade-out (`0x14002a220`) & item/prop pruning via `advance_mission_lifetimes`; `mbnetPlayer::initialize`/`clear`/`disconnectPlayer`/`startMission` (`0x1401b7220`/`0x1401b7580`/`0x1401a8c20`/`0x14017c650`/`0x14017c7c0`) wired into `465`/`466`/`468`/`2904`/`416`; regenerated `docs/SYMBOL_MAP.json` via `build_symbol_map.py`; extended `savegame_test`, `input_test`, `block_vm_test`, `native_m5_test`, and `native_world_test`. |
| Phase 2A savegame review fixes | PASS (Linux clang++ 18: header-only gate loop, CMake + CTest; g++ 13 ASan/UBSan `savegame_test`; `check_citations.py --self-test --docs`; merged in `ca0163a` + Windows PASS) | Fixed in review of `9cc4431` (`d997c18` PR #21). **`RglString`:** moved-from reset; **`savegame.h`:** transactional loads, troop merge, item ammo only, `flags & 0xff` icon persist/restore, allocation bounds, corrupt record / out-of-range keys, torn-write byte compare, saturating casts, `map_zoom_delta` frame dedupe, and restored P5 strings. All 7 remaining §5 review items + Windows verification are closed in the entry above. |
| Phase 2A/2C/2G savegame container, early-out & `rglString` + P8 `map_zoom_delta` consumer | PASS (Windows host + `check_citations` + `extract_wse_opcodes`; CI run `43930482530` green on `9cc4431`) | Added clean-room `wse/core/rgl_string.h` (`RglString` 48-byte SSO + `getNextToken` `0x14030e810` + `swap*` `0x14030f240`/`0x14030f270` + `"-1"` sentinel), `BinaryChecksumWriter` + `compute_savegame_checksum` (`wse/io/binary_stream.h`, `rglOutputBinaryFileStreamWithChecksum` `0x140565b40`), `write_simple_trigger_savegame` (`wse/game/trigger.h`), whole `.sav` container framing (`WRDR` `0x52445257`, `[802..1174]` version bounds, `g_moduleVersion` `0x14061f0c8` / `g_compatibleSavegameModuleVersion` `0x14061f0c0`, `mbSavegame::readStream` `0x1400c73b0`, `fillSavegameHeader` `0x1400e1e30`, `mbLoadSaveWindow::updatePanels` `0x140113c10`, `mbGame::save` `0x1400e16c0` VFS staging/backup, and all `mbGame::{writeStream,readStream}` `0x1400c9a90`/`0x1400c8520` section codecs in `wse/game/savegame.h`, with `bandit_attraction` %<->float scale via `0x1401d248c`), `mbSimpleTriggerManager::execute` `0x1402101a0` positive-odd-key type-`0x12` early-out over `world.m5.enqueued_windows` (`wse/vm/block_vm.h`), and `WorldInputBridge` `map_camera_distance` `exp(-map_zoom_delta)` consumer (`wse/game/world_input.h`); added `tools/build_symbol_map.py` entries + regenerated `docs/SYMBOL_MAP.json`; extended `savegame_test.cpp` (`12` funcs), `block_vm_test.cpp` (`21` funcs), and `input_test.cpp` (`8` funcs) |
| Slice P8 review fixes | PASS (`e8b3594` PR #20: Windows `build_host.ps1` `21/21` `15,580 ms` + retail `wse_brf` `Native` `221/221` & `Phantasy Calradia` `333/333` `spirv_fail=0` + `wse_headless` `20` frames / `755` parties; CI green) | Fixed in review of `9411644`. **Android:** `WseActivity` had dropped `getLibraries()`, so `SDLActivity` would load the nonexistent `libmain.so` at launch (restored `SDL3` + `wse2`); headroom polls use `SystemClock.elapsedRealtime`. **Thermal:** a reading without headroom (status listener callbacks, rate-limited `getThermalHeadroom`) was read as "cool" and dropped the tier, so the tier flapped between 10 s polls; the last valid headroom is now held for up to `16` such updates (`ThermalPerformanceController::effective_headroom`), and recovery steps down one tier per update instead of cascading Critical -> Nominal at once. **Settings:** `save_performance_settings` and `save_input_profile` failed on every fresh `HostVfs` / `SafVfs` (`config/`, `input_profiles/` missing; `MemoryVfs` creates parents, hiding it) — new `make_parent_dirs` (`wse/platform/vfs.h`). **Gyro:** SDL gyro axes are in the natural orientation, so landscape play swapped yaw/pitch; `gyro_look_rates` remaps per `ScreenRotation` (current minus natural `SDL_DisplayOrientation`, refreshed on `SDL_EVENT_DISPLAY_ORIENTATION`); smoothing is per 60 Hz frame scaled by `dt` (was frame-rate dependent), the last rate is held `50 ms` between sensor samples (a 50 Hz gyro at 120 Hz stuttered), and `invert_look_y` no longer also flips the gyro. **Pinch:** `map_zoom_delta` is `ln(spread ratio)` with a `0.02` minimum spread (was a linear distance difference). **Shader lowering:** `mat*` / `sampler*` type checks are exact (a parameter named like `uniform mat4 matWorld` was rewritten into a block member: syntax error), `for (int k ...)` scopes `k` to the loop, `isampler*` / `usampler*` get bindings (were put inside the std140 block), raw-path `#define`s rename `gl_FragData`, and successful compiles keep glslang parse warnings. **Added:** compatibility-profile built-ins (`gl_Vertex`, `gl_Color`, `gl_TexCoord[]`, `gl_ModelViewProjectionMatrix`, `ftransform()`, ...; §5 "P5 lowering review" 3), GLSL 1.30 file-scope `in` / `out`, `FrameRateCap::Fps30`, `FrameTimeStats` (measured FPS / 1% low in the SDL footer), controller gyro via `SDL_SetGamepadSensorEnabled` (preferred over the phone sensor), `ANativeWindow_setFrameRateWithChangeStrategy(ALWAYS)` for the launcher's choice, launcher caps filtered to the display's refresh rates, and `wse_brf --compile-shaders --jobs N` (parallel, sorted output). Open items: §5 "P8 review" |
| Slice P8 Sensors & performance hooks | PASS (Windows host + `aa754f6` PR #19 retail verification; CI & device pending push) | Verified `aa754f6` (`PR #19` `69883c0` merge): `Engine CI` `4/4` PASS (run `43875772112`), clean `libwse_glslang.a` rebuild in `engine/build_host.ps1` (`21/21` / `7414 ms`), and `271/271` retail `Native` & `Phantasy Calradia` shader compiles at generator `4` (`spirv_fail = 0`). Implemented Slice P8: `engine/include/wse/platform/performance.h` (`FrameRateCap` 60/90/120 Hz + uncapped, `PresentModePreference`, `ThermalPerformanceController` hysteretic thermal status & headroom scaling -> resolution scale / LOD bias / FPS step-down, drift-free `FramePacer`, `RuntimePerformanceHub`, persisted JSON settings), opt-in filtered `Mission` gyro aiming + `CampaignMap` two-finger pinch-to-zoom (`wse/platform/input.h`, `wse/game/world_input.h`), `SDL_INIT_SENSOR` `SDL_SENSOR_GYRO` (`platform/sdl/sdl_input.{h,cpp}`), Android `PowerManager` thermal listener/`getThermalHeadroom` + `ANativeWindow_setFrameRate*` + SDL3 loop pacing (`platform/sdl/sdl_selftest_main.cpp`, `platform/android/saf_vfs_jni.cpp`, `WseActivity.kt`, `WseSafBridge.kt`), persisted launcher toggles (`WseLauncherActivity.kt`), plus `test_p8_sensors_and_pinch_zoom` (`input_test.cpp` `8` funcs) and `test_p8_performance_and_frame_pacing` (`platform_test.cpp` `13` funcs); full `engine/build_host.ps1` (`21/21` / `158` functions, `selftest_host` `7371 ms`) PASS |
| Slice P5 lowering review fixes | PASS (`PR #19` `69883c0` -> `aa754f6`, `Engine CI` `4/4` PASS run `43875772112` + Windows `build_host.ps1` `21/21` + `271/271` retail shaders at generator `4` PASS) | Fixed in review of `6b85ed7`. `wse/asset/shader.h`: varying locations and sampler bindings now come from both stages (`partner_glsl`) in sorted-name order, sized by type and array (were per-stage first-come, so unknown names mismatched across stages and `mat3`/arrays overlapped; colliding fixed names such as `Color`/`VertexColor` were rejected); `compile_warband_glsl_to_spirv` runs glslang's preprocessor first (top-level `#if` alternatives were hoisted apart, macros naming uniforms / `texture2D` / `gl_FragColor` broke, `\` continuations rejected); comments now become a space; uniform initializers reported as `uniform_defaults` instead of garbage members; locals / parameters / struct members shadowing a uniform no longer renamed; struct-typed uniforms declared after their struct; float exponents kept whole; `bool` uniforms kept native (bool arrays broke); `shadow2D*` keep the `vec4` result; `textureCube*`, `texture3D*`, `*ARB` lookups, `gl_FragData[n]`, `invariant`/`centroid varying`, and `# version` handled. Reflection: Warband member names, `std140` byte sizes, array / matrix strides (were element counts and mangled names), filled on cache hits from a `.refl` sidecar. Cache keyed by content (shared GLSL compiles once) with the glslang version in the tag. `wse_shader_compiler.cpp`: compiles no longer serialized behind one global mutex; SPIR-V builder log captured; `buildReflection` checked. `750`/`751` share `camera_projection_scales` (aspect fallbacks differed); dead `omitted` parameter removed from `native_world_check_key_state`; `wse_brf --compile-shaders` infers the vertex declaration and reports each stage; glslang C-interface sources dropped; CMake build-info variables scoped. |
| Slice P5 GLSL → SPIR-V 1.0 & P7 v1.1.5.5 follow-ups | PASS (Windows host + retail `Native` & `Phantasy Calradia`; CI & device pending push) | Replaced placeholder `emit_placeholder_spirv_module` in `wse/asset/shader.h` with `lower_warband_glsl_to_vulkan450` (attributes `0..8`, graph-colored 9-location varyings `0..8`, per-stage `std140` UBOs at binding `0`/`1` with `bool` → `uint`, samplers `2..13`, `gl_FragColor`, `texture2D*`) + SHA-256-pinned Khronos `glslang 16.6.0` (`wse_glslang` + `wse_shader` isolated behind `wse/asset/shader_compiler.h`, `kWseSpirvGeneratorVersion = 3` `OpString` tag validation); `asset_test.cpp`, `platform_test.cpp` (real `VkShaderModule` + reflection), and `wse_brf.exe --compile-shaders` compile `271/271` retail Warband GL/WSE2 techniques (`0` failures) + warm VFS cache reuse (`compile_count == 0`). Resolved P7 §5 #1–#4 from v1.1.5.5 disassembly (`0x1400eedf0` chords, `0x140057010` unclamped pitch in `wse/asset/audio.h`, `0x140332970`/`0x1402ff7b0`/`0x1402ff940` `f`=up/`-u`=forward camera & UI `[0,1]x[0,0.75]` projection in `native_world_ops.h`/`native_ui_ops.h`, `0x1401c69b5` `omit_key_once` leaving raw script `70`/`71` active + same-frame release+repress fix in `wse/platform/input.h`); full `engine/build_host.ps1` (`21/21` / `155`, `selftest_host` `5922 ms`) PASS |
| Slice P7 review fixes | PASS (`4057d1e`, `Engine CI` `4/4` PASS run `43851983887` + Windows `build_host.ps1` `21/21` PASS) | Fixed in `wse/platform/input.h`: analog touch/gamepad movement snapping to full speed past `stick_key_threshold` (synthesized keys double-counted) and unnormalized diagonals; key/button taps and attack/block touch taps inside one frame lost; every other consecutive wheel notch lost; buttons stuck after a gamepad unplug, and one pad's idle axes zeroing another's; nothing released on focus loss; physical game-key clicks not following `keyClicked`/`keyRaised` (`0x1400eee90`/`0x1400eef50`); `omit_key_once` never consumed; gamepad stick not driving `MapMove*`; non-circular stick and swipe/radial/look skew on non-square screens; mouse look in menus; overlay hidden for good on devices that always report a controller; profile names with punctuation or UTF-8 refused; locale-dependent `%g` profile floats; unbounded free-camera yaw. In `wse/game/world_input.h`: `75` cursor Y inverted and in the wrong range (now Warband UI space), `3101`/`3102` missing same-frame taps, omit state not round-tripped, 190 key names re-formatted every frame. In `platform/sdl/sdl_input.cpp`: `FINGER_CANCELED`, focus/background, Android Back, flipped wheel, mouse-synthesized touches, failed `SDL_OpenGamepad`, stale first-frame viewport; `sdl_selftest_main.cpp` passes the real frame time. In `wse/platform/audio.h`: a sound started from an end callback could evict a slot whose queued end then hit the new owner (no eviction while dispatching). Remaining device/design items: §5 "P7 review". |
| Binary savegame format | PASS | `rglBinaryStream` raw little-endian 8/4/1-byte values; strings are `int32` length + bytes; no header or checksum |
| Module stream identification | PASS | `0x1405651e0` = `rglInputTextFileStream` (readers at `+0x58/+0x70/+0x78/+0x80/+0x90`); `0x1405653e0` = `rglOutputTextFileStream` writers; module files are decimal token text |
| Host PanVK loader/surface test | PASS | `host loader tests: PASS` (legacy renderer path, unaffected) |
| Portable engine host tests | PASS | `operand tests: PASS`, `trigger tests: PASS`, `savegame tests: PASS`, `script tests: PASS`, `wse_bitwise_fp tests: PASS`, `wse_system tests: PASS`, `wse_string tests: PASS`, `wse_dict tests: PASS`, `wse_array tests: PASS`, `wse_world tests: PASS`, `block_vm tests: PASS`, `PASS native_world_test`, `PASS native_m5_test`, `PASS wse_m6_test`, `platform tests: PASS`, `module_loaders tests: PASS`, `headless tests: PASS`, `lua tests: PASS`, `asset tests: PASS`, `selftest_host tests: PASS (19/19, 3549 ms)` (Zig clang++ `-std=c++20 -O2 -Wall -Wextra -Werror`) |
| Slice P7 Input + touch overlay & P6 v1.1.5.5 follow-ups | PASS (Windows host + retail Warband audio; CI & device pending push) | Verified v1.1.5.5 `mbAgent::playSound` (`0x140057010` `m_partyNo`, `0.3f`, `1/(1-delta)` negative branch, player bypass), `0x140332d80` / `0x140332f80` audio flags, and fixed re-entrant `flush_channel_ends()` sibling-slot reuse (`audio_test.cpp` regression). Reconstructed v1.1.5.5 `rglInputManager::getKeyName` (`0x1402ad360`, 125 raw keys) & `mbGameKeyManager` (`0x14069fbd0`, `0x1400e96a0`: 75 built-ins + Custom1..20 + 13 gamepad alts; `keyDown`/`keyClicked`/`keyRaised` `0x1400eedf0`/`0x1400eee90`/`0x1400eef50`; `getKeyData` `0x1400ef190`; `3100` `0x1401e6a42`) in `wse/platform/input.h`, `wse/game/world_input.h`, and `platform/sdl/sdl_input.{h,cpp}`; added `InputContext` visibility, gamepad auto-hide, multi-touch stick/look/4-dir attack & block swipes/command radial, `TouchLayoutEditor`, per-module `Vfs` JSON profiles, `FreeCameraController` driving the P4 `BrfMeshRenderer` viewer, `WorldInputBridge` (`65`, `70..73`, `75`, `77..78`, `3100..3102`), and `input_test.cpp` (`6` functions; `21/21` suites / `154` tests) |
| Slice P6 review fixes | PASS (Linux host: standalone gate `18/18` + `script`, CMake CTest `21/21` incl. `audio` + `selftest_host`, g++ ASan/UBSan CTest `21/21`; Windows, NDK/APK CI & device pending push) | Fixed in `wse/platform/audio.h`: null `shared_ptr` dereference when a channel-end callback stopped or evicted channels mid-mix (callbacks now queued and delivered once consistent); `set_config` silently ending every channel; `std::clamp` UB on `min_distance_3d > max_distance_3d` (config sanitised); `play_track` of the playing track stopping it and then dropping the request; random start limited to frame `< 32767`; streamed music interpolating against a clamped last frame at every 2048-frame refill (max error `0.0166` -> `7e-10` vs predecoded); loop points not interpolated; backwards clock stalling the scheduler and fades; `last_sound_channel` disagreeing with `615`. In `wse/asset/audio.h`: `compute_agent_voice_pitch` signed overflow (now wrapping `uint32_t`); 5.1 centre to left only and LFE to right only (speaker-position fold); `0xFFFFFFFF` WAV `data` size rejected; AAC ADTS taken for MP3. Opcodes `1881`/`1750` leaked replaced channels; agent/prop sounds never moved; voice pitch never applied; failed sample decodes retried on every play. Performance: no allocation per mix block or stream refill (member scratch buffers, decoders write into the caller's buffer, in-place fold, no `drmp3_get_pcm_frame_count` scan, reserved Vorbis decode), `nth_element` instead of a full sort, no gain/pan work for virtual voices (100-voice bench `1.45%` -> `1.07%` of real time on the host); gain ramps + soft clipper; `pump_to_latency`. CMake re-hashes vendored `stb_vorbis.c` / `dr_mp3.h` on every configure. |
| P4/P5 review follow-ups & Windows retail check | PASS (Windows host + retail Warband; Android NDK/APK CI & device pending push) | Fixed `face_shader_high` base typo (synthetic test + `kWse2SkinMappings[0]`, all 5 WSE2 variants incl. `face_shader_skin_high` synthesize with `0` warnings); confirmed `v1.1.5.5` `render_order` nibble (`0x1402b9e50`, `0x1402b9ef5`), blend `7` vertex-alpha dispatch (`0x1402e913f`), blended `D3DRS_ALPHAREF` (`0x1402e8439`/`0x1402a00b0`), `kShaderFlagHasSpecular` `0x20` gate (`0x1402e8454`), shader-only gamma (`0x1402c02b0`), and `FrontFace::counter_clockwise` (`0x1402e8ccd`/`0x14025ac20`/`0x140501b60`, `rhi_material.h` fixed); fixed composed Vulkan `1.0` + WSI refusal fallback loop (`driver_loader.{h,cpp}`), `rhi_brf.h` `>= 65536` vertex guard, `saf_vfs_jni.cpp` display-name control-char sanitization, and `WseLauncherActivity.kt` atomic ICD read-only backup swap; `engine/build_host.ps1` (`19/19` / `140` tests, `3549 ms`), retail `wse_brf.exe` (`Native` & `Phantasy Calradia`), `check_citations.py --self-test --docs`, and `extract_wse_opcodes.py --check` all PASS |
| Slice P5 review fixes | PASS (`PR #16`; Windows/retail gaps now closed above) | Fixed: `<techniques>` root swallowing the first technique; catalog never failing (5 variants always synthesized, so a Windows install without `GLShaders/` rejected nearly every shader); placeholder SPIR-V presented as a translator (renamed `emit_placeholder_spirv_module`, P5 reopened); wrong VAs in this file and `docs/ARCHITECTURE.md`; WSI detected but not enabled (now instance WSI + `VK_KHR_swapchain`, with fallback); exponential alternative search (now BFS + `SuitableShaderResolver`); SPIR-V cache accepting truncated/foreign modules (full validation, source-hash/stage/generator check, `.partial` + rename); draws failing after legal shader-module destroy; silent `VkShaderModule`-less success; allocating `noexcept` feature scan + substring false positives; INI BOM/duplicate keys; PostFx `#if WSE2` substring test; `uses_specular` inconsistency; half-pixel clip helper ignoring `w`/Y convention; object-space lighting and arbitrary tangents (now `MeshDrawCall::model` + UV tangents); D3D9 alpha-channel blending; NaN UNORM writes; stage/entry-point validation; `rhi_material.h` converters; retail test path via `WSE_WARBAND_ROOT`; `wse_brf` memoized/names-only/no copies. Not changed: `SYMBOL_MAP.json` churn/date (needs the PDB to regenerate) and the `Dev <dev@localhost>` author of `6be524d` (history is pushed). |
| Slice P5 shaders, materials & D3D9/Vulkan RHI | PASS (host + retail Warband modules; CI & device pending) | Verified v1.1.5.5 effect search (`0x14028bce0`, `0x14029c930`, `0x1402db690`), `wse2_shaders.{brf,ini}` fallbacks (`0x1402eeec0`, `0x1402ed3c0`), shader suitability & alternatives (`0x140301ab0`, `0x1402f3db0`, `0x1402f3eb0`, `0x1402f1ef0`), material flags/slots (`0x1402f5390`, `0x14029f590`, `0x1402a00b0`, `0x1402e7f50`), and `VertexDeclType` `0..4` (`0x1402baa30`) in `docs/SYMBOL_MAP.json`; added `engine/include/wse/asset/shader.h` (effect/INI/shader/material resolution, `GLShaders` catalog + 5 WSE2 skinning variants, deterministic SPIR-V 1.0 generator, persistent `SpirvShaderCache`), extended `wse/platform/rhi.h` + `platform/vulkan/driver_loader.{h,cpp}` (`PresentationPath` `android_surface_wsi` vs `offscreen_hardware_buffer_bridge`, `VkShaderModule` lifecycle via `MockVulkanIcd` v5 + `VulkanRhiDevice`, D3D9 RH perspective/ortho/half-pixel/sRGB helpers, Blinn-Phong lighting + normal/specular maps + alpha-test-before-depth-write + 5 blend modes in `SoftwareRhiDevice`), and extended `wse_brf`, `asset_test.cpp` (`8` tests), and `platform_test.cpp` (`10` tests); `build_host.ps1` (`19/19` / `137` tests, `8473 ms`) and retail `wse_brf.exe` PASS on `Native` (`221/221`, `warnings=0`, `gl_tech=271`, `144` shaders [`111` suitable, `33` fallback], `633/633` resolved materials, `58` INI pairs) and `Phantasy Calradia` (`333/333`, `warnings=0`, `gl_tech=271`, `144` shaders, `1,963/1,963` resolved materials) |
| Lua 5.1 runtime (Phase 2F) | PASS | `liblua51_cxx.a` (Lua 5.1.5 MIT compiled as C++) + `wse_lua` (`engine/src/lua/*.cpp`), `wse/vm/lua_{runtime,manager}.h`, `lua_bridge.h` (`5100`–`5114`), `block_vm.h` (`TriggerIterationScope`, `execute_statement` / `execute_script` hooks), `headless_runner.h` + `wse_headless.cpp`; verified across all 6 suites in `lua_test.cpp` (stdlib/bit/vector/pos, `msfiles` + `variables.txt` parsers & `execOperation`, entity iterators + triggers/hooks, opcodes `5100`–`5114` & 9 `On*` events, sandboxed `HostVfs` headless `main.lua` execution, and stack/defer/unwind/io/shutdown regressions) |
| Phase 2H headless `Native` run | PASS (host local) | `wse_headless.exe` against user retail install (`Modules/Native`): `34` loaded files + `1` skipped optional `module_config.ini` (after fixing `particle_systems.txt` 35-float layout `0x140207d50`, `skins.txt` wrapping u32 color literals `0x1402afce0` + `(weight, key_index)` constraint pairs `0x140213d60`, and `Data/skyboxes.txt` scalar `flare_strength` `0x1402180c0`); `game_start` (`idx 0`, `maxDepth 5`) + `20` simulated frames across `25` triggers and `110` simple triggers (`500.0s`, resulting in `755` parties) completed with `errorCount=0` and `unimplementedCount=0` under `failOnUnimplemented=true` |
| Slice P4 Vulkan bring-up & driver loader | PASS (host + Linux/arm64 objects; CI & device pending) | Inspected `FristOneRR-Panvk-Driver` (`v3.7.0`) and `Banners-Turnip` (`v27.0.0-20261115`) AdrenoTools layouts; added `wse/platform/rhi.h` (`RhiDevice`, `SoftwareRhiDevice`, `parse_driver_meta_json`, `patch_icd_libhardware_needed`, `stage_driver_package_to_vfs`, BRF/DDS textured mesh helpers), `platform/vulkan/driver_loader.{h,cpp}` (`VulkanDriverLoader`, `MockVulkanIcd` v5, `VulkanRhiDevice`, `run_vulkan_mesh_smoke_test`), `libpanvk_hw.so` (`panvk_hw` CMake/Gradle target from `runtime/android/src/hardware_stub.c`), and `nativeProbeVulkanAndRenderMesh` launcher button; fixed Windows NTFS `HostVfs::cache_key` ASCII case normalization; `platform_test.cpp` (`9` tests) and full `build_host.ps1` (`19/19` / `135`, `1717 ms`) PASS on host |
| Slice P4 review fixes | PASS (Linux host: CMake CTest `20/20`, standalone gate `19/19`, g++ ASan/UBSan; Windows, CI & device pending) | Launcher probe never reached a real driver (empty ICD path + forced mock fallback reported as success): added `.zip` driver-package import (`nativeStageDriverPackage` -> `noBackupFilesDir/gpu_drivers/<mode>/`, read-only ICD), mock fallback now opt-in and flagged (`fell_back_to_mock`); ELF-aware `patch_icd_libhardware_needed` (`DT_NEEDED` + `DT_VERNEED` only) and non-ELF rejection; `VulkanDriverLoader` `HMI` hwvulkan HAL path, `RTLD_NODELETE`, instance `apiVersion` up to 1.3, BC feature + `maxImageDimension2D` probing, `vkDeviceWaitIdle`, move-assignment teardown, Mesa `MESA_SHADER_CACHE_DIR` / PanVK env; `SoftwareRhiDevice` near-plane + guard-band clipping, fixed-point top-left fill rule, perspective-correct colours, no per-draw copies; BRF corner de-duplication; JNI UTF-16 string conversion; `wse_vulkan` CMake library; `AGENTS.md` Linux gate links the loader for `platform` |
| Slice P4 second review fixes | PASS (Linux host: standalone gate `19/19`, CMake CTest `20/20` with header checks, g++ ASan/UBSan CTest `20/20`; Windows cross-compile + link via Zig; Kotlin type-checked against the Android 14 framework jar; device pending) | Stale Vulkan handles from an earlier device no longer destroy a live object (`device_generation`); driver staging never truncates an existing file in place (`.partial` + `Vfs::rename_file`, `meta.json` last) and reports an uncreatable stage directory; `libraryName` allow-list; nested `meta.json` members skipped; `minApi` / requested-mode check; exact driver-mode ids (launcher stores ids, migrates labels); ICD interface version range check; Windows System32-only system loader + UTF-8 custom paths; `HostVfs` folded cache keys with a same-directory check (fixes case-distinct directories sharing a listing; covers macOS / casefolded volumes); `PipelineDesc::front_face`; empty frame on failed pass; `clear_depth` validation; handle reuse guard after wrap; orbit camera far plane; cached `BrfMeshRenderer`; `rhi.h` split into `rhi.h` / `driver_package.h` / `rhi_brf.h`; mock ICD object accounting; probe in a separate `:vkprobe` process; host system-Vulkan tests opt-in (`WSE_TEST_SYSTEM_VULKAN=1`). Open items: §5 "P4 review: items not fixed yet" |
| Slice P3 / Phase 2G asset decoders | PASS (host local, re-verified after PR #12 / `0629bdf`) | `wse/asset/{brf,dds}.h` + `wse_brf.exe` against retail Warband install (`7e60f1d`, including `dwDepth` and empty-root fixes): `CommonRes` (`221/221` `.brf`: `7,243` meshes, `2,939` metameshes, `734` textures, `144` shaders, `633` materials, `856` bodies, `2` skeletons, `203` animations, `1` physics material), `WSE2 SDK` (`3/3` `.brf`: `12` meshes, `12` metameshes, `5` shaders, `1` physics material), `Textures` (`1,269/1,269` `.dds` incl. `L16_UNORM` skybox exponent maps and single-slice `DDSCAPS2_VOLUME` `cnt/font.dds`), and `Modules/Native` resource catalog (`221/221` entries, `0` warnings) |
| Slice P2 SAF Vfs & launcher | PASS (host + JNI/Kotlin build; device pending) | `wse/platform/saf_vfs.h` (`SafVfs`, `SafTreeBackend`, `MemorySafBackend`, `scan_warband_modules`, `verify_module_scripts`, `import_to`), `platform/android/saf_vfs_jni.cpp`, `WseSafBridge.kt`, `WseLauncherActivity.kt`: `platform_test.cpp` verifies `check_contract`, zero-rescan index behavior (`list_children_calls == 0`), `Modules/*/module.ini` discovery, `scripts.txt` parse via `SafVfs` (`4` scripts, `4` statements, `1` mapped), and subtree `import_to`; on-device SAF index timing (`index_ms`) reported live in `WseLauncherActivity` once hardware is attached |
| Statement-block VM & trigger/script bridge | PASS | `engine/include/wse/vm/block_vm.h` + `native_*.h` reconstructed and host-tested (`analyze`, `execute`, `startLoop`/`endLoop`/`canLoop`, live entity iterators `11/12/16/17`, `try_for_dict_keys` `0x12`, `16` control-flow + `1,037` switch native ops across `native_ops.h`/`native_entity_ops.h`/`native_mission_ops.h`/`native_campaign_ops.h`/`native_campaign_ops_2.h`/`native_world_ops.h`/`native_world_ops_2.h`/`native_world_ops_3.h`/`native_agent_ops.h`/`native_mp_ops.h`/`native_scene_ops.h`/`native_ui_ops.h` + `7` non-switch/fallback via `dispatch_native_non_switch`, `call_script` up to depth 255 preserving `currentCaller`, `store_script_param*`, `this_or_next`/`neg`, script & trigger execution) |
| Game-world runtime state | PASS | `engine/include/wse/game/world_state.h` + `world_subsystem_types.h` + `world_m5_types.h` + `world_m6_types.h` + `world_campaign_helpers.h` model PDB-verified entity/slot, troop, skill, item/mesh, faction, quest/note, info page, site/visitor, party/stack, player, agent, prop, team, presentation/overlay, audio/battle event, mission timer, skybox/light/postfx/ray-obstacle, config, anticheat/net-buffer/server/Lua bridge, and edit-mode containers (`WseWorldState` wired into `VmContext::world` and `WseVmState::world`) |
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
