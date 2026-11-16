# STATUS

Last updated: 2026-11-16.
Phase: **2 — Core Data Structures & Module System VM** (Phase 1 triage is complete; see `ROADMAP.md`).

This file is the **only** place that holds live numbers (gate results, coverage counts). Other
documents link here instead of repeating them — see the ownership table in `AGENTS.md` §4.

## 1. At a glance

| Area | State |
| --- | --- |
| Evidence baseline | WSE2 **v1.1.5.5** x64 (PDB + PE + Ghidra 12.1.4). The 2026-10-14 build is superseded. |
| Host gate | **PASS** on Windows after Slice P5 GLSL → SPIR-V 1.0 & P7 v1.1.5.5 evidence follow-ups (`engine/build_host.ps1` with `WSE_WARBAND_ROOT`, Zig `clang++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror`: standalone headers incl. `shader_compiler.h` + `wse_lua` + `wse_audio` + `wse_glslang`/`wse_shader` + `21/21` suites / `155` test functions + `selftest_host` `21/21` `5922 ms` + `wse_headless` + `wse_brf`; retail `Native` & `Phantasy Calradia` `271/271` GL/WSE2 shader techniques compiled to SPIR-V 1.0 with `0` failures). Linux CI (`Engine CI`) passed `4/4` on `4057d1e` (run `43851983887`); re-runs with `spirv-val` on push |
| Android arm64 gate | **PASS (CI run `43851983887` on `4057d1e`; re-runs on push)** — `.github/workflows/ci.yml` (`android-arm64` + `android-apk`) compiles all headers standalone, cross-compiles CMake targets (`engine`, `lua51`, `wse_lua`, `wse_vulkan`, `wse_audio`, `wse_glslang`, `wse_shader`, `wse_selftest`), and builds `arm64-v8a` debug APK (`libwse2.so` + `libpanvk_hw.so` + `libSDL3.so` + `assets/scripts.txt` with `WseLauncherActivity` + `WseSafBridge`); on-device runtime tests remain unrun |
| WSE extended opcodes | 440/440 mapped, **440/440 implemented** |
| Native (non-WSE) opcodes | **1,053 live handlers mapped** (`16` control-flow `0–29` + `1,037` switch `30–2767`, plus `6` `noop_true` and `1` SDK-named fallback `1004`; `3` live handlers `53, 2180, 2749` have no SDK constant and carry community names); **1,053/1,053 implemented (100%, 20/20 categories)** (`16` control-flow incl. 4 live entity iterators + `1,037` switch across `block_vm.h`, `native_ops.h`, `native_entity_ops.h`, `native_mission_ops.h`, `native_campaign_ops.h`, `native_campaign_ops_2.h`, `native_world_ops.h`, `native_world_ops_2.h`, `native_world_ops_3.h`, `native_agent_ops.h`, `native_mp_ops.h`, `native_scene_ops.h`, and `native_ui_ops.h`) |
| Module loaders | Clean-room module loaders implemented across `wse/game/module_{loader_common,world_loaders,asset_loaders,mission_loaders,bundle}.h` reading exclusively via `wse::platform::Vfs` + `wse::io::TokenReader` (`skills`, `factions`, `troops` v1/v2, `item_kinds` v1/v3, `party_templates`, `parties`, `scenes`, `quests`, `info_pages`, `strings`, `quick_strings`, `game_variables`, `module.ini`, `module_config.ini`, `meshes`, `actions`, `skins`, `sounds` v2/v3, `music`, `particle_systems`, `postfx`, `map_icons`, `scene_props`, `menus`, `presentations`, `mission_templates`, `tableau_materials`, `conversation` v1/v2, `scripts`, `triggers`, `simple_triggers`, `Data/{item_modifiers,flora_kinds,ground_specs,skyboxes}.txt`) |
| World state & headless run (Phase 2H) | `WseWorldState` populated from module files via `wse::game::load_module` (`module_bundle.h`), binding `ScriptManager`, triggers, and owned UTF-16 `WseStringBanks` into `VmContext` / `WseVmState`; **Phase 2H headless runner** (`wse/game/headless_runner.h` + `engine/tools/wse_headless.cpp`) validated locally on user retail `Modules/Native` (`34` loaded files, `1` skipped optional `module_config.ini`, `game_start` + `20` trigger frames across `25` triggers and `110` simple triggers -> `755` parties, `0` VM errors, `0` unimplemented opcodes with `failOnUnimplemented = true`) |
| Lua 5.1 layer (Phase 2F) | **Complete (`lua51` + `wse_lua`)** — stock MIT Lua 5.1.5 (`2640fc56…`) wired behind `vm::WseLuaRuntime` (`wse/vm/lua_runtime.h`, `wse/vm/lua_manager.h`, `engine/src/lua/`) with `<lua.h>` isolated from headers, sandboxed `Vfs`/`Clock`/`LogSink` routing (`%storage%`, `%savegames%`, bytecode `\x1b` rejection), `bit` + clean-room `LuaGlobals`, `msfiles/` + `variables.txt` parsers, 36 `game.*` + 7 `_G.*` callbacks, trigger/presentation/psys registration, operation/script hooks, all 9 engine `On*` events, opcodes `5100`–`5114`, and `wse_headless` integration |
| Platform track (Phase 3/4) | Planned as slices P0–P9 in `docs/PLATFORM_PLAN.md`. **P0, P3, and P5 host path complete; P1, P2, P4, P6, and P7 built and host-verified, on-device run pending hardware**: `wse/platform/{vfs,saf_vfs,clock,sleep,log,rhi,driver_package,rhi_brf,audio,input}.h`, `platform/vulkan/driver_loader.{h,cpp}`, and `platform/sdl/{sdl_audio,sdl_input}.{h,cpp}` implement the VFS (`MemoryVfs`, Windows/POSIX case-insensitive `HostVfs`, `SafVfs`), clocks/sleep/logging, minimal RHI (`RhiDevice`, `SoftwareRhiDevice`, `PresentationPath` selection, D3D9 RH projection/half-pixel/sRGB helpers, `ShaderModuleDesc`/`VkShaderModule` lifecycle, Blinn-Phong + normal/specular/alpha-test/5 blend modes), AdrenoTools/PanVK driver package importer + ELF `.dynstr` `libpanvk_hw.so` patcher, `VulkanDriverLoader` (`load_system`, direct-ICD v5 / custom driver preload, `MockVulkanIcd` v5, `VulkanRhiDevice`), stereo 2D/3D audio mixer + music scheduler (`WseAudioMixer` + `NullAudioDevice`/`CaptureAudioDevice` + `SDL_OpenAudioDeviceStream`), `WseGameKeyManager` / `WseInputManager` (`InputContext`, virtual stick/look/4-dir attack & block swipes/command radial, `TouchLayoutEditor`, per-module `Vfs` JSON profiles, `FreeCameraController`, SDL3 event backend), and `wse/game/{audio_runtime,world_audio,world_input}.h` (`WorldAudioBridge` for sound opcodes + `WorldInputBridge` for `65`, `70..73`, `75`, `77..78`, `3100..3102`); `platform/android/saf_vfs_jni.cpp` + Kotlin `WseSafBridge` + `WseLauncherActivity` implement `ACTION_OPEN_DOCUMENT_TREE`, `SafVfs`, `Modules/*/module.ini` scanner, GPU driver selector, `scripts.txt` verifier, `import_to`, and the `"Probe Vulkan Driver & Render Test Mesh"` probe; `wse/asset/{brf,dds,shader,shader_compiler,audio,audio_codecs}.h` + `src/audio/` (`wse_audio` with `stb_vorbis` v1.24 + `dr_mp3` v0.7.5) + `src/shader/` (`wse_shader` with Khronos `glslang 16.6.0`) + `wse_brf` decode all 8 `.brf` resource types, `.dds` textures (incl. `BC1`–`BC5` and `L16`), `module.ini` catalogs, effect search order, `wse2_shaders.{brf,ini}`, material render state, `GLShaders` XML/GLSL + 5 WSE2 skinning variants, Warband GLSL → Vulkan `#version 450` SPIR-V 1.0 compilation, persistent `SpirvShaderCache` (`kWseSpirvGeneratorVersion = 3`), and WAV/OGG/MP3 audio streams/samples; `wse_selftest` aggregates all `21` core suites (`selftest_host_test` PASS on host); `platform/sdl/sdl_selftest_main.cpp` + `android/` Gradle (CMake targets `wse2`, `panvk_hw` + SDL3 `3.4.20` Prefab AAR) build the `arm64-v8a` APK. Sensor/frame-cap slice (`P8`) not started yet |

## 2. Gates

| Gate | Result | Detail |
| --- | --- | --- |
| Host C++20, Windows | PASS | `engine/build_host.ps1`, Zig `clang++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror`: `liblua51_cxx.a` + `wse_lua` + `libwse_audio.a` (`stb_vorbis` + `dr_mp3`) + `libwse_glslang.a` + `libwse_shader.a` (`glslang 16.6.0`) + `driver_loader` + `21/21` standalone suites / `155` test functions (`operand`, `trigger`, `savegame`, `script`, `wse_bitwise_fp`, `wse_system`, `wse_string`, `wse_dict`, `wse_array`, `wse_world`, `block_vm`, `native_world`, `native_m5`, `wse_m6`, `platform`, `module_loaders`, `headless`, `lua`, `asset`, `audio`, `input`) + `selftest_host_test.exe` (`21/21`, `5922 ms`) + `wse_headless.exe` + `wse_brf.exe` |
| Host C++20, Linux | PASS (CI run `43851983887` on `4057d1e`; re-runs on push with `spirv-val`) | `.github/workflows/ci.yml` (`ubuntu-24.04`, clang++, CMake + CTest `-DWSE_BUILD_HEADER_CHECKS=ON`): standalone header checks, CTest (`21` suites + `selftest_host`, each suite compiled once into `wse_selftest`), `spirv-val --target-env vulkan1.0`, `extract_wse_opcodes.py --check`, and `check_citations.py --self-test --docs` |
| Android NDK `aarch64-linux-android29` | PASS (CI run `43851983887` on `4057d1e`; re-runs on push) | `.github/workflows/ci.yml` (`ubuntu-24.04`, NDK Clang, `-std=c++20 -O2 -Wall -Wextra -Werror`): all `73` headers pass standalone compile + CMake cross-compile build; job `android-apk` (`gradle -p android assembleDebug` with pinned NDK / CMake -> `libwse2.so`, `libSDL3.so`, `libpanvk_hw.so`, and `assets/scripts.txt` as the only asset) builds and uploads the APK artifact |
| Host PanVK loader/surface test (legacy) | PASS (recorded) | `host loader tests: PASS`; independent of the reconstructed engine |
| Host ASan + UBSan | PASS (CI run `43851983887` on `4057d1e`; re-runs on push) | `.github/workflows/ci.yml` job `host-sanitizers` (`ubuntu-24.04`, clang++, `cmake -DWSE_SANITIZE=ON` + CTest). First-party code is fully instrumented; the unmodified third-party `lua51` target is built with `-fno-sanitize=float-cast-overflow` only (stock `lua_number2int` in `ltable.c`). PR #9 was merged with this job failing on `6f83abc` / `7be8d6d` / `fea4229`; `b6bb82b` restored it |
| CI | PASS (run `43851983887` on `4057d1e`; re-runs on push) | `.github/workflows/ci.yml` (`Engine CI`): runs `host-linux` (incl. `spirv-val --target-env vulkan1.0`, `extract_wse_opcodes.py --check`, and `check_citations.py --self-test --docs`), `host-sanitizers`, `android-arm64`, and `android-apk` on push and PR; all four jobs passed on `4057d1e` |

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
| `savegame_test.cpp` | 8 |
| `script_test.cpp` | 5 |
| `wse_bitwise_fp_test.cpp` | 11 |
| `wse_system_test.cpp` | 8 |
| `wse_string_test.cpp` | 3 |
| `wse_dict_test.cpp` | 3 |
| `wse_array_test.cpp` | 2 |
| `wse_world_test.cpp` | 5 |
| `block_vm_test.cpp` | 21 |
| `native_world_test.cpp` | 7 |
| `native_m5_test.cpp` | 5 |
| `wse_m6_test.cpp` | 7 |
| `platform_test.cpp` | 12 |
| `module_loaders_test.cpp` | 6 |
| `headless_test.cpp` | 5 |
| `lua_test.cpp` | 7 |
| `asset_test.cpp` | 9 |
| `audio_test.cpp` | 8 |
| `input_test.cpp` | 7 |
| `selftest_host_test.cpp` *(aggregate runner over the 21 suites)* | 21 suites |
| **Total (21 standalone suites + aggregate runner)** | **155** |

## 4. Blocked

| Blocker | Impact | Way out |
| --- | --- | --- |
| No local Android NDK/SDK, `adb`, or device | CI runs the NDK arm64 compile gate on Linux; on-device Android runtime tests remain unrun | Keep `.github/workflows/ci.yml` enforcing the arm64 build; add `adb`/device validation later in Phase 3/4 |
| REA not registered with OpenCode | REA/Ghidra tools are not exposed through MCP; the CLI works from `%TEMP%\opencode\tools\rea\node_modules\rea-agents` | Keep using the CLI |

## 5. Next concrete tasks (ordered)

1. **Engine lifetime & shared value types (Phase 2A/2E/2G):** Phase 2F Lua 5.1 (`WSELuaManager` + stock Lua 5.1.5), Phase 2H headless `Native` execution, and Slice P3 / Phase 2G `.brf` + `.dds` resource decoders (`wse/asset/{brf,dds}.h`) are complete on host. Remaining Phase 2 core work covers whole-savegame framing (2A), live agent/party/mission-object/player lifetimes (2E), and shared `rglString` semantics (2G).

Platform track, in parallel (`docs/PLATFORM_PLAN.md`):

1. ~~**P5 GLSL → SPIR-V translator:**~~ **Done:** `emit_placeholder_spirv_module` has been replaced by `lower_warband_glsl_to_vulkan450` (`wse/asset/shader.h`) + Khronos `glslang 16.6.0` (`wse/asset/shader_compiler.h`, `wse_shader`), with `SpirvShaderCache` (`kWseSpirvGeneratorVersion = 3`) persisting validated SPIR-V 1.0 binaries and `271/271` retail Warband techniques compiled on host.
2. **P8 Sensors + performance (SDL3 / Android):** P0–P7 are built and host-verified (`P7 Input + touch overlay` verified across Warband `125` raw keys / `95` game keys, `InputContext`, gamepad hiding, virtual stick/look/4-direction attack & block swipes/command radial, `TouchLayoutEditor`, per-module JSON profiles, `FreeCameraController` driving the P4 viewer, `WorldInputBridge`, and `platform/sdl/sdl_input.{h,cpp}`); the next platform slice is `P8` (gyro aim toggle, campaign map pinch-zoom, thermal headroom throttling, 60/90/120 Hz frame caps).
3. **Device verification of P1/P2/P4/P6/P7 APK:** install the `wse2-android-selftest-debug-apk` CI artifact on an `arm64-v8a` device/emulator once available and record live on-device SAF index timing, SDL3 audio playback, touch/gamepad input, plus system / Turnip / PanVK driver probe results from `WseLauncherActivity`.

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
6. **`SafVfs` has no `rename_file`.** Staging through it falls back to remove + write (a short
   window without the file). Driver staging uses `HostVfs`, so nothing depends on it yet; a
   `DocumentsContract.renameDocument`/`moveDocument` backend call would close it.
7. **Non-ASCII directory case.** `HostVfs` cache keys fold ASCII only, so two spellings of a
   non-ASCII directory that differ only in case (on a case-insensitive volume) get separate
   listings: correct, one extra scan each. Unicode case folding was not worth a dependency.

### P5 review: items not fixed yet

Found in the review of `6be524d` and still open after the fixes in §7 ("Slice P5 review fixes").
Each needs a translator choice, the retail PDB/binary, hardware, or a change to pushed history.

1. ~~**No GLSL → SPIR-V translator.**~~ **Resolved (`glslang 16.6.0`):** `emit_placeholder_spirv_module` was removed; `lower_warband_glsl_to_vulkan450` (`wse/asset/shader.h`) + `create_glslang_shader_compiler()` (`wse/asset/shader_compiler.h`, `engine/src/shader/wse_shader_compiler.cpp`) lower Warband GLSL 1.10/1.20 (`attribute` → fixed locations `0..8`, `varying` → conflict-free 9-location mapping `0..8`, per-stage `std140` UBOs at binding `0` VS / `1` FS with `bool` → `uint`, samplers at bindings `2..13`, `gl_FragColor`, `texture2D*`) to `#version 450` and compile into Vulkan SPIR-V 1.0 (`kWseSpirvGeneratorVersion = 3`, encoded in a Section 7(a) `OpString` tag verified by `inspect_spirv_module` and Khronos `spirv-val --target-env vulkan1.0` in CI). All `271/271` retail Warband techniques (`542` VS+FS stages) compile cleanly with `0` failures and feed real `VkShaderModule` objects (`platform_test.cpp`); P5 is ticked `[x]` in `ROADMAP.md`.
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

1. **Audio is not wired into the app.** Nothing outside `audio_test` constructs
   `WorldAudioBridge` or pumps a device; `platform/sdl/sdl_selftest_main.cpp` only opens and closes
   the SDL stream. The embedder still has to: call `WorldAudioBridge::update` per frame, feed the
   listener from the camera (`listener_from_camera_transform`), drive `pump_to_latency` from one
   thread, and connect Android pause/resume to `AudioDevice::set_paused`.
2. **Decoding and mixing run on the caller's thread.** Lazy sample decode inside a play opcode is a
   synchronous file read + decode (preload or a worker thread would remove the hitch), and the
   mixer has no locking, so it must not move to SDL's audio callback thread without a command
   queue.
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
| Citation check (B7) | PASS | `tools/check_citations.py --self-test --docs` (`884` doc VA references across 6 living docs checked against `SYMBOL_MAP.json`, with `tools/docs_citation_baseline.txt` holding `36` pre-existing uncurated image-base/`.data`/historical VAs) validates every `0x14…` address and rejects superseded `ev_*` / synthetic addresses across all `73` headers (`1,988` VA refs / `1,643` unique VAs) under `engine/include/` (validated against committed maps and `v1155/` PDB/globals/PE ranges) |
| Tracking documents | PASS | `AGENTS.md`, `ROADMAP.md`, `docs/ARCHITECTURE.md`, `docs/SYMBOL_MAP.json` (83 classes, 3,466 symbols, 76 vtables, 4,759 function ranges, 20 global objects, 2,888 curated superseded VAs), v1.1.5.5 target |
| v1.1.5.5 REA VM/stream batches | PASS | 75 procedures across 9 `decompile_all` exports (`stmt_core` `ev_3ebcea85…` 4/4, `stmt_block` `ev_ffc38aa5…` 10/10, `script` `ev_8d37a0c8…` 4/4, `simple_trigger` `ev_3157906d…` 12/12, `trigger` `ev_f62843ac…` 12/12, `game_scripts` `ev_fc9bd1a4…` 4/4, `game_triggers` `ev_052f2d86…` 2/2, `streams_bin` `ev_8cc48986…` 9/9, `streams_txt` `ev_1e3c2566…` 18/18), plus `read_bytes`/`0x140615ad0` (`ev_f09662c6…`/`ev_2ddf7a62…`) and 6 single-procedure exports |
| Superseded 2026-10-14 REA runs | PASS (superseded) | Historical 2026-10-14 entries kept for provenance only: `ev_7cd99ccc…157d7a` (`mbScript` 5), `ev_8d91b175…99b11` (`mbStatement` 14), `ev_25469ff4…84af8` (3), `ev_e3530f99…e8d05` (`Trigger` 52), `ev_cebc7ae0…e8c8d3` (`rglTextStream` 18), `ev_24eecfa4…f18c0b6` (`rglBinaryStream` 9), `ev_130d8023…2c62e6` (2) |
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
