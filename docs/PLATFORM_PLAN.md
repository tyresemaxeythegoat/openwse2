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

**Order.** P0 → P1 → P2 are strict. P3 is host-only and can run beside P1/P2. P4 needs P1; P5
needs P3 + P4. P6, P7, P8 need only P1 and can interleave. P9 needs Phase 2E and 2H progress.

**Working beside the VM track.** Opcode and VM work owns `engine/include/wse/vm/**` and its
tests. Platform slices add new directories and touch VM headers only in the P1 routing step, which
should land as its own commit when no VM refactor is in flight.

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
