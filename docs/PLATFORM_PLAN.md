# Platform Plan — Phase 3 (cross-platform replacement) and Phase 4 (Android features)

This file owns the **design** of the platform track: the seam, the target layout, the slice
order, and the risks decided up front. The checklist and its tick marks live in
[`../ROADMAP.md`](../ROADMAP.md); gate results and what has actually run live in
[`../STATUS.md`](../STATUS.md).

## Decisions

| Decision | Choice | Why |
| --- | --- | --- |
| Sequencing | Phase 3/4 runs **in parallel** with Phase 2 as a platform track | Device risks (SAF speed, custom Vulkan drivers, shader translation) are found early; the first slices need no game world |
| Windowing, input, sensors, audio device | **SDL3** | Maintained line; Android lifecycle, gamepad, sensor, touch and audio-stream APIs |
| Graphics API | **Vulkan only**, through our own small RHI | One backend to make correct; no GLES fallback |
| GPU driver | User-selectable at runtime: system driver, **PanVK** on Mali, **Turnip** on Adreno | The stock Mali/Adreno drivers vary by vendor build; Mesa drivers are consistent |
| Renderer shape | Clean-room `rglRenderer` reconstruction drawing through the RHI | Not D3D9 kept alive through a translation layer |
| Assets and drivers | Bring-your-own, picked on device | `AGENTS.md` §1 and §3: no game files and no driver binaries in this repository |

Driver packages the user has pointed at (to be imported at runtime, never committed):
PanVK — `FristOneRR/FristOneRR-Panvk-Driver` (`v3.7.0`, AdrenoTools/Winlator `.zip` with `meta.json` +
`libvulkan_panfrost.so` exporting ICD v5 entrypoints and `DT_NEEDED: libhardware.so`, rewritten in-place to
`libpanvk_hw.so`); Turnip — `The412Banner/Banners-Turnip` (`v27.0.0-20261115`, AdrenoTools `.zip` with `meta.json`
`schemaVersion: 1`, `libraryName: "libvulkan_freedreno.so"`, `minApi: 28`, `vendor: "Mesa"`).

## Design principles

1. **One seam: `wse/platform/`.** Engine code never calls SDL, Vulkan, JNI, or (for new code)
   `std::filesystem` directly. It calls small interfaces: `Vfs`, `Clock`, `LogSink`, and later
   `Rhi`, `AudioDevice`, `Input`. Host tests get in-memory or null implementations, so every slice
   is testable in the existing gate.
2. **Every slice ends in something runnable** on the host (CTest) and on arm64 (CI compile, then
   an APK).
3. **Evidence before code.** File formats and renderer behaviour come from the v1.1.5.5 loaders
   (`rglMesh`, `rglMaterial`, `rglRenderer`, `rglConfig`; `ARCHITECTURE.md` §8 gaps 4–5), not from
   recollection of community tools. Each reconstruction slice starts with its REA export. The
   seam itself is port infrastructure, mirrors no retail layout, and says so in each header.
4. **Bring-your-own at runtime.** Game install, modules, and GPU driver are all user selections.

## Target layout

```
CMakeLists.txt                 root: engine (INTERFACE), tests (CTest), wse_selftest, Android libwse2.so
engine/include/wse/platform/   vfs.h saf_vfs.h clock.h sleep.h log.h rhi.h driver_package.h rhi_brf.h rhi_scene.h rhi_ui.h audio.h input.h performance.h (present)
engine/include/wse/asset/      brf.h, dds.h, shader.h, shader_compiler.h, skeleton.h, font.h, audio.h, audio_codecs.h (portable, host-tested)
engine/src/audio/              wse_audio_internal.hpp, wse_audio_vendor.cpp, wse_audio_codecs.cpp (`stb_vorbis` + `dr_mp3` isolated inside `wse_audio`)
engine/src/shader/             wse_shader_compiler.cpp (Khronos `glslang 16.6.0` isolated inside `wse_shader`)
platform/sdl/                  SDL3 on-device self-test shell (`FramePacer`, `ANativeWindow_setFrameRate*`) + `sdl_audio.{h,cpp}` + `sdl_input.{h,cpp}` (gamepad/touch/`SDL_SENSOR_GYRO`)
platform/vulkan/               driver_loader.{h,cpp}: Vulkan loader, MockVulkanIcd v5, VulkanRhiDevice (present)
platform/android/              saf_vfs_jni.cpp over JNI (SAF VFS + Vulkan probe + thermal/perf launch config hooks)
android/                       Gradle project: Kotlin WseLauncherActivity (SAF + module/driver picker) + WseSafBridge + WseActivity (SDLActivity) + Prefab SDL3 arm64-v8a APK (present)
third_party/                   SDL3 (Gradle Prefab), Lua 5.1 (CMake FetchContent), `stb_vorbis` + `dr_mp3` + `glslang 16.6.0` (CMake SHA-256 download), Bullet, volk/VMA — pinned by hash
```

The legacy `runtime/android/src/vk_panvk_loader.{c,h}` and `hardware_stub.c` are the starting point
for the PanVK path of the driver loader. They are referenced from the new build rather than moved;
the legacy tree stays intact (`AGENTS.md` §3).

## Slices

Each slice is one milestone. "Done when" is the acceptance test, not a description.

| # | Slice | Contents | Done when |
| --- | --- | --- | --- |
| P0 | Build + gates | Root CMake + CTest, CI host job, CI NDK `aarch64-linux-android29` compile of every header (backlog B1) | CI green on `main` |
| P1 | Platform seam + app shell | `wse/platform/{vfs,clock,sleep,log}.h` with host implementations and tests; engine file/clock/sleep routing through the seam; `wse_selftest` runner across all 18 core suites; Gradle `android/` + `platform/sdl/` SDL3 `SDLActivity` + `libwse2.so` on-device self-test screen + CI debug APK (`arm64-v8a`) | APK artifact built by CI (`android-apk`) and `selftest_host` passes on host; on-device run prints PASS when installed on a device or emulator |
| P2 *(host + APK verified; device pending)* | Launcher + SAF file system *(Phase 4)* | `wse/platform/saf_vfs.h` (`SafVfs`, `MemorySafBackend`, `scan_warband_modules`, `verify_module_scripts`), `platform/android/saf_vfs_jni.cpp`, and Kotlin `WseSafBridge` + `WseLauncherActivity`: `ACTION_OPEN_DOCUMENT_TREE` for the Warband folder, persisted URI permission, module list from `Modules/*/module.ini`, mod switcher, GPU driver picker (`System`/`Turnip`/`PanVK`), one-time case-insensitive tree index, reads through `openFileDescriptor`, `scripts.txt` verification, and `import_to(Vfs&)` to app storage. | Host `SafVfs` tests (`platform_test.cpp`) and Android APK build pass in CI; on-device launcher lists modules from a user tree, parses `scripts.txt` through `SafVfs`, and records live index build time (`STATUS.md` §7) |
| P3 *(complete)* | Asset decoders (host-first) | PE/PDB evidence exports (`docs/SYMBOL_MAP.json`) and `wse/asset/{brf,dds}.h`: `.brf` (mesh, material, texture, shader, skeleton, animation, collision body, physics material), `.dds` (BC1–BC5 + uncompressed RGBA/BGRA/BGR/16-bit/L8/L16/A8L8/A8 + DX10 + Warband texture search chain), and `module.ini` resource catalogs. Closes the `.brf`/`.dds` work of Phase 2G. | Host tests (`asset_test.cpp`) and `wse_brf` CLI pass on synthetic fixtures and 100% of retail Warband `CommonRes`, `WSE2 SDK`, `Textures`, and `Modules/Native` (`STATUS.md` §7) |
| P4 *(host verified; CI & device pending)* | Vulkan bring-up + driver loader | Both driver packages inspected (`v3.7.0` PanVK + `v27.0.0-20261115` Turnip). `engine/include/wse/platform/rhi.h` (`RhiDevice`, `SoftwareRhiDevice`), `driver_package.h` (`meta.json` parser, ELF `.dynstr` `libpanvk_hw.so` patcher, atomic staging), `rhi_brf.h` (BRF/DDS textured mesh helpers, `BrfMeshRenderer`), `platform/vulkan/driver_loader.{h,cpp}` (`VulkanDriverLoader`, `MockVulkanIcd` v5, `VulkanRhiDevice`), `libpanvk_hw.so` (`platform/android/panvk_hw_stub.c`), and the `nativeProbeVulkanAndRenderMesh` probe run in the isolated `:vkprobe` process. | Textured static mesh screenshot test (`SoftwareRhiDevice` vs `VulkanRhiDevice` over `MockVulkanIcd` v5; real host loader opt-in) PASS on host; the frame is still CPU-rendered, so the GPU render path and on-device runs with real drivers remain open (`STATUS.md` §5) |
| P5 *(host verified; device pending)* | Shaders + materials | PE/PDB evidence for v1.1.5.5 effect search, `rglShader`, `rglMaterial`, `GLShaders` XML/GLSL pairs, and WSE2 variants (`docs/SYMBOL_MAP.json`); `engine/include/wse/asset/{shader,shader_compiler}.h` + `wse_shader` (effect/INI/shader/material resolution, `GLShaders` catalog + 5 WSE2 skinning variants, `lower_warband_glsl_to_vulkan450` + Khronos `glslang 16.6.0` Vulkan SPIR-V 1.0 compiler with `OpString` tag validation and persistent `SpirvShaderCache`); `engine/include/wse/platform/rhi.h` + `platform/vulkan/driver_loader.{h,cpp}` (`PresentationPath`, `ShaderModuleDesc`/`VkShaderModule` lifecycle, D3D9 RH projection/half-pixel/sRGB, Blinn-Phong + normal/specular/alpha-test/5 blend modes in `SoftwareRhiDevice`/`VulkanRhiDevice`). | Host `platform` + `asset` suites, CI `spirv-val --target-env vulkan1.0`, and retail `wse_brf` pass on `Native` + one large mod (`STATUS.md` §7: `271/271` techniques compiled); second launch hits persistent SPIR-V cache with zero compiles; real-GPU pipeline execution pending device |
| P6 *(host verified; device pending)* | Audio | PE/PDB evidence for v1.1.5.5 `mbSoundSample` / `mbSoundManager` / `mbMusicManager` (`docs/SYMBOL_MAP.json`); `wse/asset/{audio,audio_codecs}.h` + `wse_audio` (`stb_vorbis` v1.24 + `dr_mp3` v0.7.5 + clean-room RIFF/WAVE PCM decoder, 4-step module/language sound lookup), `wse/platform/audio.h` (`AudioDevice`, `NullAudioDevice`, `CaptureAudioDevice`, `WseAudioMixer` 2D/3D constant-power stereo mixer, LOD fallback, priority eviction, occlusion hysteresis, and `0.5s`/`5.0s` music scheduler), `wse/game/{audio_runtime,world_audio}.h` (`WorldAudioBridge` + VM opcodes `599..604`, `609`, `615..616`, `1750`, `1808`, `1881..1882`, `3618`), and `platform/sdl/sdl_audio.{h,cpp}` (`SDL_OpenAudioDeviceStream` backend + startup audio probe). | Host `audio` suite (`audio_test.cpp`) mixes deterministically through `CaptureAudioDevice` and passes on synthetic `.wav`/`.ogg`/`.mp3` + retail Warband (`Native` & `Phantasy Calradia`); SDL3 APK links `sdl_audio.cpp`; on-device speaker playback pending hardware |
| P7 *(host verified; device pending)* | Input + touch overlay *(Phase 4)* | PE/PDB evidence for v1.1.5.5 `rglInputManager::getKeyName` (`0x1402ad360`, 125 raw keys) & `mbGameKeyManager` (`0x14069fbd0`, `reset` `0x1400e96a0`: 75 built-ins + Custom1..20 + 13 gamepad alts); `wse/platform/input.h` (`WseGameKeyManager`, `InputContext`, touch overlay with movement stick/look area/4-direction attack & block swipes/command radial, `TouchLayoutEditor`, per-module `Vfs` JSON profiles, `FreeCameraController`), `wse/game/world_input.h` (`WorldInputBridge` opcodes `65`, `70..73`, `75`, `77..78`, `3100..3102`), and `platform/sdl/sdl_input.{h,cpp}`. | Host `input` suite (`input_test.cpp`) passes driving `FreeCameraController` in the P4 `BrfMeshRenderer` viewer, hiding in `Menu`/`Inventory`/`Presentation` contexts and on controller attach; SDL3 APK links `sdl_input.cpp`; on-device touch/gamepad test pending hardware |
| P8 *(host verified; device pending)* | Sensors + performance *(Phase 4)* | `wse/platform/performance.h` (`FrameRateCap` 30/60/90/120 Hz + uncapped, `PresentModePreference`, `ThermalPerformanceController` hysteretic thermal status & headroom scaling -> resolution scale / LOD bias / thermal FPS step-down, with immediate escalation, one-tier-per-update recovery, and the last valid headroom held across headroom-less readings, `FramePacer`, `FrameTimeStats`, `RuntimePerformanceHub`, persistent `Vfs` JSON settings), opt-in filtered gyro aim (frame-rate-independent smoothing, sample-and-hold, axes remapped for the screen rotation; controller gyro preferred when present) + `CampaignMap` two-finger pinch-to-zoom as `ln` of the spread ratio (`wse/platform/input.h`, `wse/game/world_input.h`), `SDL_INIT_SENSOR` `SDL_SENSOR_GYRO` + `SDL_SetGamepadSensorEnabled` (`platform/sdl/sdl_input.{h,cpp}`), SDL3 pacing + `ANativeWindow_setFrameRate*` (`ALWAYS` for the player's choice, `ONLY_IF_SEAMLESS` for thermal step-downs) and a measured-FPS footer (`sdl_selftest_main.cpp`), Android `PowerManager` thermal listener + `getThermalHeadroom` (`WseActivity.kt`, `saf_vfs_jni.cpp`), and persisted launcher toggles with caps filtered to the display's refresh rates (`WseLauncherActivity.kt`). | Host `platform` + `input` suites pass across gyro/pinch/hysteresis/frame-pacer tests and Android APK builds cleanly; each toggle exists in JSON + UI; on-device frame-rate & gyro test pending hardware |
| P9 *(parts 1 & 2 host verified)* | Convergence with Phase 2 | Part 1 landed: skeletal FK & CPU linear-blend skinning (`wse/asset/skeleton.h`, fixing `skeleton_anim` `(qx,qy,qz,qw)` read order in `wse/asset/brf.h`), `map.txt`/`.sco`/`skeleton_bodies.xml` loaders (`wse/game/scene_loaders.h`), and chunked terrain + multi-object/skinned `BrfSceneRenderer` over `RhiDevice` (`wse/platform/rhi_scene.h`, static IB + slot skin-data cache). Part 2 landed: shared XML DOM parser (`wse/io/xml.h`), `Data/font_data.xml` glyph loader & `<c=...>`/word-wrap text layout (`wse/asset/font.h`), `LanguageTable` / `GameVariablesTable` / `UiWidget` / `UiSession` initial+menu+notes+presentation controllers (`wse/game/ui_runtime.h`), and 2D UI RHI renderer (`wse/platform/rhi_ui.h`, Y-up `[0,1]x[0,0.75]` ortho & bottom-up V -> top-down DDS `1 - v` UV conversion) tested in `ui_test.cpp`. Lua 5.1 link (2F) and 2H headless runner integrated. Remaining: Bullet physics hookup and on-device `Native` run. | Scene/terrain/skeletal and `Native` main menu + menu/presentation RHI passes on host; `Native` main menu renders on device from a user-supplied install |
| P10 *(planned; P10.0 done)* | UI/UX and performance on Android *(Phase 4)* | UI aspect modes and crisp text, asynchronous start with a loading screen, performance hub in module mode, drawn touch overlay with context buttons and gestures, touch-friendly menus, soft keyboard, texture budget, launcher settings, performance-core hints; details in "P10 — UI/UX and performance on Android" below | The device acceptance list in that section holds on the user's phone |

**Order.** P0 → P1 → P2 are strict. P3 is host-only and can run beside P1/P2. P4 needs P1; P5
needs P3 + P4. P6, P7, P8 need only P1 and can interleave. P9 needs Phase 2E and 2H progress.

**Working beside the VM track.** Opcode and VM work owns `engine/include/wse/vm/**` and its
tests. Platform slices add new directories and touch VM headers only in the P1 routing step, which
should land as its own commit when no VM refactor is in flight.

## P10 — UI/UX and performance on Android

**Why.** The first device run (2026-11-18, APK `64250bd`) showed the module UI working, with
three complaints: the UI looks ugly, module mode started in portrait, and a wrong folder gave no
clear error. Portrait and the error dialog are fixed (P10.0 below); the rest is this slice.

**Reference study (clean-room boundary).** The user supplied a third-party Android build of
Warband (`mnb_noobb` v4.2) as the example of how the UI should look and how the controls should
work. Antigravity (Gemini 4 Hydrogen XHigh, read-only) studied only its control and config text
files and the strings in its Java code, which Claude extracted into the session scratchpad; no
native library was opened, and no code, data, layout coordinates or assets were copied. What it
showed, in our words:

- *2D UI aspect.* Warband's UI is a 4:3 canvas. Stretching it over a 20:9 phone (what we do
  today) distorts every mesh and glyph; the reference keeps menus, dialogs and pause screens at
  4:3 or 16:9 (letterboxed) and lets only the 3D map and battles fill the screen, with an option
  to keep touch buttons inside the picture.
- *Touch controls* are a context-gated overlay sized as a fraction of the screen's short side and
  placed in percent of width / height: a stick bottom-left, a large attack pad bottom-right
  (slide to pick the direction), defend / kick / jump / use / zoom around it, small weapon,
  camera and menu buttons in the top rows, map-mode buttons (home / follow, pan mode, fast travel,
  pause), trade / party bulk-move buttons, dialog up / select / down, and user-assignable spare
  buttons. Safe-area insets keep them off notches.
- *Menus on touch* need the first tap to register, drag-to-scroll on long lists, and drag and
  drop committed on finger lift.
- *Settings* the reference exposes: render scale, FPS cap, quality presets, texture size cap,
  UI aspect mode, message / font scale, control layout and sensitivity, language.
- *Performance:* conservative mobile engine defaults on first launch (30 FPS cap, low texture
  detail, small battles), on-demand texture loading, a texture size cap and cache, loading spread
  over frames with a progress screen, and keeping the game threads on performance cores.

**Work units.** Claude writes each unit's spec and its host tests first; Antigravity implements
the unit (`agy-handoff --effort xhigh --dir . --tests <the new tests>`); Claude reviews the diff,
runs the host gate, and the user checks each batch on the phone. Units marked *Claude* need
judgement or retail evidence and are not delegated.

| Unit | What | Files | Owner | Done when |
| --- | --- | --- | --- | --- |
| P10.0 | Landscape-only module mode; missing-file check with an error dialog | `sdl_selftest_main.cpp`, `WseActivity.kt`, `WseLauncherActivity.kt`, `ui_game_shell.h` | Claude | **Done** (`find_missing_warband_files`, `SDL_ShowSimpleMessageBox`, launcher `AlertDialog`); device check pending |
| P10.1 | UI aspect modes: 4:3 (default for 2D windows), 16:9, stretch; letterbox bars; pointer mapped through the same rectangle | `ui_game_shell.h` (viewport rect in `UiRenderConfig`), `ui_frame.h` (`UiInputAccumulator` inverse map), `rhi_geometry.h` (offset batches), `performance.json` key | agy | A tap on a letterbox bar hits nothing; a 2400x1080 frame keeps a 1440x1080 4:3 picture centred; host tests |
| P10.2 | Crisp UI: per-texture filter (nearest for pixel-exact UI art at 1:1, linear when scaled), font atlas never downscaled, optional UI text scale | `rhi_geometry.h` (filter hint per batch), `sdl_selftest_main.cpp`, `rhi_ui.h` | agy | Batch carries a filter hint; text scale applied in `measure_text` and layout; host tests |
| P10.3 | Loading screen and asynchronous start: SAF indexing and `UiGameShell::start` on a worker thread, progress drawn by the SDL loop, no ANR | `ui_game_shell.h` (`start_async`, progress), `sdl_selftest_main.cpp`, `saf_game_tree.h` | Claude spec, agy | The SDL loop keeps presenting during a 10 s synthetic load; cancel on quit; host test with a slow VFS |
| P10.4 | Performance hub in module mode: launcher FPS cap, thermal step-down, present mode (today fixed at 60 Hz); 30 FPS default for menus with idle presentation skipping already in place | `sdl_selftest_main.cpp` (`run_module_shell`), `performance.h` | Claude | Cap and thermal tier follow `RuntimePerformanceHub`; footer shows measured FPS in a debug overlay |
| P10.5 | Draw the touch overlay (stick, attack pad, buttons, radial) as UI geometry, sized from the short side, clamped to `SDL_GetWindowSafeArea` | `input.h` (layout in short-side units), `rhi_ui.h` / new `touch_overlay_render.h`, `ui_game_shell.h` | agy | Overlay batches appear only in contexts that use them; safe-area clamp test |
| P10.6 | Context buttons and gestures: delayed swipe classification for attack / block, two-level order radial, map buttons (home / follow / pan / fast travel / pause), dialog up / select / down, trade and party bulk buttons, spare assignable buttons | `input.h`, `world_input.h` | agy (P7 review item 6 spec) | Each button maps to a game key in `WseGameKeyManager`; host tests per context |
| P10.7 | Touch-friendly menus: drag-to-scroll in scrollable containers, first tap counts, scissor clipping for `clip_children` | `ui_runtime.h`, `ui_frame.h`, `rhi_geometry.h` (clip rect per batch), presenter `SDL_SetRenderClipRect` | agy | Drag scrolls a 40-item list; clipped items never draw outside the box |
| P10.8 | Soft keyboard: `SDL_StartTextInput` / `SDL_StopTextInput` when a text or number box gains / loses focus | `ui_game_shell.h` (focus query), `sdl_selftest_main.cpp` | agy | Focus change toggles text input in a host test of the shell flag |
| P10.9 | Texture budget: mip clamp from a texture-detail setting and a max size; decode only the chosen mip | `dds.h` callers, `rhi_ui.h`, `rhi_scene.h`, `performance.h` | agy | A 2048 texture loads as 512 at detail 25%; memory counter in the shell |
| P10.10 | Launcher settings: UI aspect, UI scale, FPS cap, quality preset (writes conservative first-launch defaults), language from `languages/*/`; restyled launcher (dark theme, grouped cards) | `WseLauncherActivity.kt`, `performance.h`, `saf_vfs_jni.cpp` | agy | Settings round-trip through `config/performance.json`; CI builds the APK |
| P10.11 | Performance cores: Android `APerformanceHintManager` session for the game thread (API 33+), thread priority fallback | `sdl_selftest_main.cpp`, new `platform/android/perf_hint.cpp` | Claude | Hint session reports the frame target; no-op below API 33 |

**Batches.** A: P10.1, P10.2, P10.4, P10.3 (fixes "ugly" and startup). B: P10.5, P10.6, P10.7,
P10.8 (controls). C: P10.9, P10.10, P10.11. Each batch: one host gate, one push, one APK for the
user's phone.

**Device acceptance.** Launch to main menu under 5 s for Native from internal storage; menu and
presentation frames under 4 ms CPU at 60 FPS; no ANR during load; text readable at 1080p without
zooming; every touch control reachable with a thumb in landscape on a 20:9 phone.

## Risks decided up front

| Risk | Decision |
| --- | --- |
| SAF is slow for thousands of small files | Index the tree once, read through file descriptors, offer import to app storage |
| Modules reference files in the wrong case and with `\` | The `Vfs` contract is case-insensitive and accepts both separators (already true of `MemoryVfs` and `HostVfs`) |
| Custom driver loading depends on linker-namespace behaviour that varies by Android version and SoC | The system driver is always a working fallback; driver choice is a per-device setting |
| Loading a custom driver directly bypasses Android's `libvulkan.so`, which is what implements `VK_KHR_android_surface` and the swapchain (drivers only expose `VK_ANDROID_native_buffer`) | **Decided in P5 (`PresentationPath`):** `VulkanDriverLoader::probe_and_create_device` queries `vkEnumerateInstanceExtensionProperties`; System Vulkan (`!used_direct_icd && supports_android_surface_wsi`) selects `android_surface_wsi` (`VkSurfaceKHR`), whereas direct custom ICDs (`Turnip`/`PanVK` via `dlopen`) select `offscreen_hardware_buffer_bridge` (`AHardwareBuffer` + `ASurfaceControl`) |
| Imported driver packages are executable code from the user | Staged under `noBackupFilesDir/gpu_drivers/<mode>/`, ELF-checked, patched, and marked read-only (Android 14 refuses writable dynamically loaded code); never bundled |
| Modules may ship shader source, compiled effects, or both | **Decided in P5 (`wse/asset/{shader,shader_compiler}.h`):** Warband + WSE2 ship complete GLSL source/techniques in `GLShaders/*.xml` + `GLShadersOptimized/` alongside `wse2_shaders.ini`; resolve BRF shaders/materials against that catalog, synthesize the 5 WSE2 skinning variants when absent, lower GLSL 1.10/1.20 to `#version 450` via `lower_warband_glsl_to_vulkan450`, compile to Vulkan SPIR-V 1.0 via isolated Khronos `glslang 16.6.0` (`wse_shader`), and persist validated binaries in `SpirvShaderCache` |
| Mobile GPUs may not sample the block-compressed formats in `.dds` | Query format support; transcode at import and cache on disk |
| No device is attached to any build machine | P0–P3 are fully CI-verifiable; from P2/P4 on, device results come from running the CI APK and reading its self-test screen |
| Third-party licences | Permissive libraries are linked statically; any LGPL component ships only as a separate shared library; no FMOD, no Steam |
