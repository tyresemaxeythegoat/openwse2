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
3. **Evidence before code.** File formats and renderer behaviour come from the v1.1.5.6 loaders
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
| P5 *(host verified; device pending)* | Shaders + materials | PE/PDB evidence for v1.1.5.6 effect search, `rglShader`, `rglMaterial`, `GLShaders` XML/GLSL pairs, and WSE2 variants (`docs/SYMBOL_MAP.json`); `engine/include/wse/asset/{shader,shader_compiler}.h` + `wse_shader` (effect/INI/shader/material resolution, `GLShaders` catalog + 5 WSE2 skinning variants, `lower_warband_glsl_to_vulkan450` + Khronos `glslang 16.6.0` Vulkan SPIR-V 1.0 compiler with `OpString` tag validation and persistent `SpirvShaderCache`); `engine/include/wse/platform/rhi.h` + `platform/vulkan/driver_loader.{h,cpp}` (`PresentationPath`, `ShaderModuleDesc`/`VkShaderModule` lifecycle, D3D9 RH projection/half-pixel/sRGB, Blinn-Phong + normal/specular/alpha-test/5 blend modes in `SoftwareRhiDevice`/`VulkanRhiDevice`). | Host `platform` + `asset` suites, CI `spirv-val --target-env vulkan1.0`, and retail `wse_brf` pass on `Native` + one large mod (`STATUS.md` §7: `271/271` techniques compiled); second launch hits persistent SPIR-V cache with zero compiles; real-GPU pipeline execution pending device |
| P6 *(host verified; device pending)* | Audio | PE/PDB evidence for v1.1.5.6 `mbSoundSample` / `mbSoundManager` / `mbMusicManager` (`docs/SYMBOL_MAP.json`); `wse/asset/{audio,audio_codecs}.h` + `wse_audio` (`stb_vorbis` v1.24 + `dr_mp3` v0.7.5 + clean-room RIFF/WAVE PCM decoder, 4-step module/language sound lookup), `wse/platform/audio.h` (`AudioDevice`, `NullAudioDevice`, `CaptureAudioDevice`, `WseAudioMixer` 2D/3D constant-power stereo mixer, LOD fallback, priority eviction, occlusion hysteresis, and `0.5s`/`5.0s` music scheduler), `wse/game/{audio_runtime,world_audio}.h` (`WorldAudioBridge` + VM opcodes `599..604`, `609`, `615..616`, `1750`, `1808`, `1881..1882`, `3618`), and `platform/sdl/sdl_audio.{h,cpp}` (`SDL_OpenAudioDeviceStream` backend + startup audio probe). | Host `audio` suite (`audio_test.cpp`) mixes deterministically through `CaptureAudioDevice` and passes on synthetic `.wav`/`.ogg`/`.mp3` + retail Warband (`Native` & `Phantasy Calradia`); SDL3 APK links `sdl_audio.cpp`; on-device speaker playback pending hardware |
| P7 *(host verified; device pending)* | Input + touch overlay *(Phase 4)* | PE/PDB evidence for v1.1.5.6 `rglInputManager::getKeyName` (`0x1402ad5d0`, 125 raw keys) & `mbGameKeyManager` (`0x1406a0c00`, `reset` `0x1400e9800`: 75 built-ins + Custom1..20 + 13 gamepad alts); `wse/platform/input.h` (`WseGameKeyManager`, `InputContext`, touch overlay with movement stick/look area/4-direction attack & block swipes/command radial, `TouchLayoutEditor`, per-module `Vfs` JSON profiles, `FreeCameraController`), `wse/game/world_input.h` (`WorldInputBridge` opcodes `65`, `70..73`, `75`, `77..78`, `3100..3102`), and `platform/sdl/sdl_input.{h,cpp}`. | Host `input` suite (`input_test.cpp`) passes driving `FreeCameraController` in the P4 `BrfMeshRenderer` viewer, hiding in `Menu`/`Inventory`/`Presentation` contexts and on controller attach; SDL3 APK links `sdl_input.cpp`; on-device touch/gamepad test pending hardware |
| P8 *(host verified; device pending)* | Sensors + performance *(Phase 4)* | `wse/platform/performance.h` (`FrameRateCap` 30/60/90/120 Hz + uncapped, `PresentModePreference`, `ThermalPerformanceController` hysteretic thermal status & headroom scaling -> resolution scale / LOD bias / thermal FPS step-down, with immediate escalation, one-tier-per-update recovery, and the last valid headroom held across headroom-less readings, `FramePacer`, `FrameTimeStats`, `RuntimePerformanceHub`, persistent `Vfs` JSON settings), opt-in filtered gyro aim (frame-rate-independent smoothing, sample-and-hold, axes remapped for the screen rotation; controller gyro preferred when present) + `CampaignMap` two-finger pinch-to-zoom as `ln` of the spread ratio (`wse/platform/input.h`, `wse/game/world_input.h`), `SDL_INIT_SENSOR` `SDL_SENSOR_GYRO` + `SDL_SetGamepadSensorEnabled` (`platform/sdl/sdl_input.{h,cpp}`), SDL3 pacing + `ANativeWindow_setFrameRate*` (`ALWAYS` for the player's choice, `ONLY_IF_SEAMLESS` for thermal step-downs) and a measured-FPS footer (`sdl_selftest_main.cpp`), Android `PowerManager` thermal listener + `getThermalHeadroom` (`WseActivity.kt`, `saf_vfs_jni.cpp`), and persisted launcher toggles with caps filtered to the display's refresh rates (`WseLauncherActivity.kt`). | Host `platform` + `input` suites pass across gyro/pinch/hysteresis/frame-pacer tests and Android APK builds cleanly; each toggle exists in JSON + UI; on-device frame-rate & gyro test pending hardware |
| P9 *(parts 1 & 2 host verified)* | Convergence with Phase 2 | Part 1 landed: skeletal FK & CPU linear-blend skinning (`wse/asset/skeleton.h`, fixing `skeleton_anim` `(qx,qy,qz,qw)` read order in `wse/asset/brf.h`), `map.txt`/`.sco`/`skeleton_bodies.xml` loaders (`wse/game/scene_loaders.h`), and chunked terrain + multi-object/skinned `BrfSceneRenderer` over `RhiDevice` (`wse/platform/rhi_scene.h`, static IB + slot skin-data cache). Part 2 landed: shared XML DOM parser (`wse/io/xml.h`), `Data/font_data.xml` glyph loader & `<c=...>`/word-wrap text layout (`wse/asset/font.h`), `LanguageTable` / `GameVariablesTable` / `UiWidget` / `UiSession` initial+menu+notes+presentation controllers (`wse/game/ui_runtime.h`), and 2D UI RHI renderer (`wse/platform/rhi_ui.h`, Y-up `[0,1]x[0,0.75]` ortho & bottom-up V -> top-down DDS `1 - v` UV conversion) tested in `ui_test.cpp`. Lua 5.1 link (2F) and 2H headless runner integrated. Remaining: Bullet physics hookup and on-device `Native` run. | Scene/terrain/skeletal and `Native` main menu + menu/presentation RHI passes on host; `Native` main menu renders on device from a user-supplied install |
| P10 *(host verified; device pending)* | Launcher UI/UX and performance on Android *(Phase 4)* | Launcher rebuilt as home / modules / settings / advanced pages with a dark theme, a first-run flow with a background scan and install check, grouped settings; asynchronous start with a loading screen, performance hub in module mode, texture budget, performance-core hints; details in "P10 — Launcher UI/UX and performance on Android" below | The device acceptance list in that section holds on the user's phone |
| P11 *(planned)* | In-game UI and touch controls on Android *(Phase 4)* | UI aspect modes and crisp text, drawn touch overlay with context buttons and gestures, touch-friendly menus, soft keyboard; details in "P11 — In-game UI and touch controls on Android" below | The device acceptance list in that section holds on the user's phone |
| P12 *(planned)* | Launcher redesign *(Phase 4)* | Jetpack Compose + Material 3 launcher with the install's own module art, dynamic colour, motion, and screenshot tests in CI; details in "P12 — Launcher redesign" below | The screenshot artifact and the user's phone check both look right |

**Order.** P0 → P1 → P2 are strict. P3 is host-only and can run beside P1/P2. P4 needs P1; P5
needs P3 + P4. P6, P7, P8 need only P1 and can interleave. P9 needs Phase 2E and 2H progress.

**Working beside the VM track.** Opcode and VM work owns `engine/include/wse/vm/**` and its
tests. Platform slices add new directories and touch VM headers only in the P1 routing step, which
should land as its own commit when no VM refactor is in flight.

## P10 — Launcher UI/UX and performance on Android

**Why.** The first device run (2026-11-18, APK `64250bd`) showed the module UI working, with
three complaints: the app looks ugly, module mode started in portrait, and a wrong folder gave no
clear error. Portrait and the error dialog are fixed (P10.0 below).

**Scope.** The UI/UX half of this slice is the **launcher** (`WseLauncherActivity`): the Android
screen where the player picks the Warband folder, a module and settings before the game starts.
Today it is one long scroll of developer controls left over from slices P2, P4 and P8 (SAF index
stats, driver import and probe, mock-ICD switch, self-test, `scripts.txt` check) with the player's
choices mixed in. P10 does **not** restyle Warband's own in-game UI; the in-game UI and touch
controls are slice P11 below. The other half of P10 is startup and runtime performance.

**Reference study (clean-room boundary).** The user supplied a third-party Android build of
Warband (`mnb_noobb` v4.2) as the example of how the app should look and how the controls should
work. Antigravity (Gemini 4 Hydrogen XHigh, read-only) studied only its control and config text
files and the strings in its Java code, which Claude extracted into the session scratchpad; no
native library was opened, and no code, data, layout coordinates or assets were copied. What it
showed, in our words:

- *Launcher.* A separate companion screen in front of the game, where the player picks the game
  folder, manages mods and language packs, and sets graphics, controls and language before
  launching. Settings sit in a few topic groups; permissions, errors and confirmations use
  ordinary Android dialogs and toasts rather than status text.
- *Settings* it exposes: render scale, FPS cap, quality presets, texture size cap, UI aspect
  mode, message / font scale, control layout and sensitivity, language.
- *Performance:* conservative mobile engine defaults on first launch (30 FPS cap, low texture
  detail, small battles), on-demand texture loading, a texture size cap and cache, loading spread
  over frames with a progress screen, and keeping the game threads on performance cores.
- *In-game* findings (UI aspect, touch controls, touch-friendly menus) belong to P11 and are
  listed there.

**Work units.** Claude writes each unit's spec and its host tests first; Antigravity implements
the unit (`agy-handoff --effort xhigh --dir . --tests <the new tests>`); Claude reviews the diff,
runs the host gate, and the user checks each batch on the phone. Units marked *Claude* need
judgement or retail evidence and are not delegated. Kotlin has no host tests, so launcher units
put any logic worth testing (settings keys, install checks, JSON shapes) in C++ headers with host
tests and keep the Kotlin side to layout and wiring; CI builds the APK and the user checks it.

| Unit | What | Files | Owner | Done when |
| --- | --- | --- | --- | --- |
| P10.0 | Landscape-only module mode; missing-file check with an error dialog | `sdl_selftest_main.cpp`, `WseActivity.kt`, `WseLauncherActivity.kt`, `ui_game_shell.h` | Claude | **Done** (`find_missing_warband_files`, `SDL_ShowSimpleMessageBox`, launcher `AlertDialog`); device check pending |
| P10.1 | Launcher structure: a home page (chosen install, chosen module card, one large Play button, last error), a Modules page, a Settings page and an Advanced page that holds every developer control (rescan and index stats, driver import and probe, mock ICD, SDL self-test, `scripts.txt` check). Framework views only; no WebView and no new dependency | `WseLauncherActivity.kt` split into page classes under `android/.../launcher/`, `res/values/` | agy | Home shows no developer control; a returning player reaches Play in one tap; CI builds the APK |
| P10.2 | Launcher look: dark theme, grouped cards, spacing and type sizes from `dimens.xml` (text in `sp`), touch targets at least 48 dp, works in portrait and landscape, errors and confirmations as `AlertDialog` / `Toast` instead of status lines | `res/values/{themes,colors,dimens}.xml`, `res/drawable/`, launcher pages | agy | No status-text error path left; layout checked on the phone in both orientations |
| P10.3 | First-run flow and module list: pick folder, scan on a worker thread with a progress bar and cancel, module cards (name, folder, install check), last module remembered; Play is disabled with the reason when the install check fails | launcher pages, `saf_vfs_jni.cpp` (install check returned as JSON), `ui_game_shell.h` (`find_missing_warband_files` result to JSON) | Claude spec, agy | Launcher never freezes during a scan; host test for the install-check JSON; a wrong folder shows the missing files before Play |
| P10.4 | Settings page grouped as Graphics (quality preset that writes conservative first-launch defaults, FPS cap, present mode, render scale, texture size cap, thermal options), Display (UI aspect, UI scale; stored now, used by P11), Controls (gyro, pinch; layout editor entry once P11 lands), Language (from the install's `languages/*/`) | launcher pages, `performance.h` (new keys), `saf_vfs_jni.cpp` | agy | New keys round-trip through `config/performance.json` in `platform_test`; first launch writes the preset defaults |
| P10.5 | Loading screen and asynchronous start: SAF indexing and `UiGameShell::start` on a worker thread, progress drawn by the SDL loop, no ANR | `ui_game_shell.h` (`start_async`, progress), `sdl_selftest_main.cpp`, `saf_game_tree.h` | Claude spec, agy | The SDL loop keeps presenting during a 10 s synthetic load; cancel on quit; host test with a slow VFS |
| P10.6 | Performance hub in module mode: launcher FPS cap, thermal step-down, present mode (today fixed at 60 Hz); 30 FPS default for menus with idle presentation skipping already in place | `sdl_selftest_main.cpp` (`run_module_shell`), `performance.h` | Claude | Cap and thermal tier follow `RuntimePerformanceHub`; footer shows measured FPS in a debug overlay |
| P10.7 | Texture budget: mip clamp from a texture-detail setting and a max size; decode only the chosen mip | `dds.h` callers, `rhi_ui.h`, `rhi_scene.h`, `performance.h` | agy | A 2048 texture loads as 512 at detail 25%; memory counter in the shell |
| P10.8 | Performance cores: Android `APerformanceHintManager` session for the game thread (API 33+), thread priority fallback | `sdl_selftest_main.cpp`, new `platform/android/perf_hint.cpp` | Claude | Hint session reports the frame target; no-op below API 33 |

**Batches.** A: P10.1, P10.2, P10.3, P10.4 (the launcher rebuild; fixes "ugly"). B: P10.5,
P10.6, P10.7, P10.8 (startup and performance). Each batch: one host gate, one push, one APK for
the user's phone.

**Status (2026-11-18).** P10.1-P10.8 landed together as one batch: each unit ran as its own
Antigravity agent on a git worktree (`repo/.wt/<unit>`, spec in `.verify/SPEC.md`), with
Claude's host tests committed first for P10.3-P10.5 and P10.7 (`headless_test`
`test_launcher_install_check_json`, `test_list_warband_languages`,
`test_ui_game_shell_start_async`; `platform_test` `test_p10_launcher_settings`; `ui_test`
`test_texture_budget`). Claude reviewed each diff, merged the branches, and ran the host gate.
The Kotlin, SDL and JNI parts compile in CI only; the device acceptance list below is still
to be checked on the user's phone.

**Device acceptance.** First setup (pick folder, pick module, Play) fits on three screens or
fewer; a returning player starts the last module in one tap; the launcher never freezes during a
scan; launch to main menu under 5 s for Native from internal storage; no ANR during load; menu
and presentation frames under 4 ms CPU at 60 FPS.

## P11 — In-game UI and touch controls on Android

**Why.** The reference study's in-game findings: Warband's UI is a 4:3 canvas, and stretching it
over a 20:9 phone (what we do today) distorts every mesh and glyph; the reference keeps menus,
dialogs and pause screens at 4:3 or 16:9 (letterboxed) and lets only the 3D map and battles fill
the screen, with an option to keep touch buttons inside the picture. Touch controls are a
context-gated overlay sized as a fraction of the screen's short side and placed in percent of
width / height: a stick bottom-left, a large attack pad bottom-right (slide to pick the
direction), defend / kick / jump / use / zoom around it, small weapon, camera and menu buttons in
the top rows, map-mode buttons (home / follow, pan mode, fast travel, pause), trade / party
bulk-move buttons, dialog up / select / down, and user-assignable spare buttons, with safe-area
insets keeping them off notches. Menus on touch need the first tap to register, drag-to-scroll on
long lists, and drag and drop committed on finger lift. P11 starts after P10; it reads the UI
aspect and UI scale that the P10.4 settings page stores. Same workflow as P10.

| Unit | What | Files | Owner | Done when |
| --- | --- | --- | --- | --- |
| P11.1 | UI aspect modes: 4:3 (default for 2D windows), 16:9, stretch; letterbox bars; pointer mapped through the same rectangle | `ui_game_shell.h` (viewport rect in `UiRenderConfig`), `ui_frame.h` (`UiInputAccumulator` inverse map), `rhi_geometry.h` (offset batches), `performance.json` key | agy | A tap on a letterbox bar hits nothing; a 2400x1080 frame keeps a 1440x1080 4:3 picture centred; host tests |
| P11.2 | Crisp UI: per-texture filter (nearest for pixel-exact UI art at 1:1, linear when scaled), font atlas never downscaled, optional UI text scale | `rhi_geometry.h` (filter hint per batch), `sdl_selftest_main.cpp`, `rhi_ui.h` | agy | Batch carries a filter hint; text scale applied in `measure_text` and layout; host tests |
| P11.3 | Draw the touch overlay (stick, attack pad, buttons, radial) as UI geometry, sized from the short side, clamped to `SDL_GetWindowSafeArea` | `input.h` (layout in short-side units), `rhi_ui.h` / new `touch_overlay_render.h`, `ui_game_shell.h` | agy | Overlay batches appear only in contexts that use them; safe-area clamp test |
| P11.4 | Context buttons and gestures: delayed swipe classification for attack / block, two-level order radial, map buttons (home / follow / pan / fast travel / pause), dialog up / select / down, trade and party bulk buttons, spare assignable buttons | `input.h`, `world_input.h` | agy (P7 review item 6 spec) | Each button maps to a game key in `WseGameKeyManager`; host tests per context |
| P11.5 | Touch-friendly menus: drag-to-scroll in scrollable containers, first tap counts, scissor clipping for `clip_children` | `ui_runtime.h`, `ui_frame.h`, `rhi_geometry.h` (clip rect per batch), presenter `SDL_SetRenderClipRect` | agy | Drag scrolls a 40-item list; clipped items never draw outside the box |
| P11.6 | Soft keyboard: `SDL_StartTextInput` / `SDL_StopTextInput` when a text or number box gains / loses focus | `ui_game_shell.h` (focus query), `sdl_selftest_main.cpp` | agy | Focus change toggles text input in a host test of the shell flag |

**Batches.** A: P11.1, P11.2, P11.5, P11.6 (menus). B: P11.3, P11.4 (controls).

**Status (2026-11-18).** P11.1, P11.3 and P11.4 landed (host-verified; device run pending),
built as Antigravity units on worktrees and reviewed by Claude. P11.1: `compute_ui_viewport`,
`UiInputAccumulator::set_viewport`, `GeometryFrame::viewport_*`, `UiGameShellOptions::ui_aspect`
(default 4:3) from `ui_aspect`; the presenter clears the window black and draws the batches at
their offset. P11.3/P11.4: widget sizes in short-side units, `apply_safe_area`, context buttons
(`MapCenter`, `MapFollow`, `MapPanMode`, `DialogUp/Select/Down`, `MoveAll`, `MoveOne`,
`Spare1..4`) with `widget_visible_in_context`, delayed swipe classification (120 ms or
`swipe_threshold`), the two-level order radial, `touch_overlay_render.h`, and
`UiGameShell::set_touch_overlay`. The module shell wires it: each frame `UiGameShell::step_input`
sets the context from the world's screen window, keeps touches that start on the visible
overlay (`WseInputManager::overlay_claims_touch`) away from the menu UI, and mirrors keys into
`world.input`; the app clamps the user's layout to `SDL_GetWindowSafeArea` whenever the window
or safe area changes, so drawing and hit-testing use the same rectangles. Map, dialog and bulk-move buttons send `Custom5..12`,
because Warband has no game key for them; a module that binds those custom keys itself will
also see them. P11.2, P11.5 and P11.6 followed in wave B:
`GeometryBatch::filter` (nearest only for axis-aligned 1:1 texel-to-pixel draws, linear
otherwise; the presenter sets `SDL_SetTextureScaleMode` per batch), `UiRenderConfig::text_scale`
from `ui_scale` (glyphs only, boxes and hit areas unchanged, so labels can overflow their box
above 1.0), touch-only drag-to-scroll and first-tap press (`UiPointerInput::touch`), scissor
clipping (`MeshDrawCall::has_clip` in UI units, `GeometryBatch::clip_*` in window pixels,
`SoftwareRhiDevice` discards outside; `VulkanRhiDevice` ignores it for now), and the soft
keyboard (`UiGameShell::text_input_wanted`, edge-triggered `SDL_StartTextInput` /
`SDL_StopTextInput`). Every P11 unit is host-verified; none has run on a device.

**Device acceptance.** Text readable at 1080p without zooming; 2D windows undistorted on a 20:9
phone; every touch control reachable with a thumb in landscape.

## P12 — Launcher redesign: Compose, Material 3 and the install's own art

**Why.** The P10 launcher works but looks flat: grey cards and stock widgets, no art, no depth,
no motion (user, 2026-11-18). The target is a cross between a console game hub and Material You:
the module's own image up front, one bold Play action, soft tonal surfaces, real motion.

**Decisions (interview, 2026-11-18).**
- Rewrite the launcher screens in **Jetpack Compose with Material 3**. This adds AndroidX
  (Compose BOM, `material3`, `activity-compose`, the Kotlin Compose compiler plugin) and about
  3–5 MB to the APK. The P10 logic stays: JNI calls, the SAF bridge, the scan / install-check
  state machine and the settings save rules move out of the activity into a `ViewModel` with
  no behaviour change.
- **Art comes only from the user's install at run time** (BYOA): every module folder has a
  `main.bmp` (the picture the PC launcher shows), and the install root has one too. Nothing
  from the game is bundled; tests use generated placeholder images.

**Design.**
- *Home:* full-bleed hero of the selected module's `main.bmp`, softly blurred and darkened
  toward the bottom, module name in large display type over it, one large Play button with
  haptic feedback, and the install status as a small chip ("Ready", "3 files missing"). In
  landscape the hero takes the left two thirds and the actions sit in a side column.
- *Modules:* a grid of cards, each with its own `main.bmp`, name and a status chip; tapping a
  card makes it the selection with a shared-element transition into Home's hero.
- *Settings:* Material 3 list items grouped under headers; segmented buttons for the quality
  preset and UI aspect, sliders with value labels for scales, switches for toggles; the
  Advanced page keeps the developer tools out of the way.
- *Colour:* dynamic colour from the wallpaper on Android 12+; below that, and as the accent
  when dynamic colour is off, a seed colour taken from the selected module's art. Dark theme
  by default, light theme supported.
- *Layout and motion:* edge-to-edge with proper insets, `NavigationBar` in portrait and
  `NavigationRail` in landscape, predictive back, fade-through between tabs.

| Unit | What | Owner | Done when |
| --- | --- | --- | --- |
| P12.1 | Compose + Material 3 build setup, `ViewModel` holding the P10 launcher state and JNI calls, the theme (dynamic colour, fallback seed, type scale) | agy (Claude spec) | APK builds in CI; the old screens still work through the `ViewModel` |
| P12.2 | Module art: decode `main.bmp` from the SAF tree (or `HostVfs`) off the main thread, cache it scaled, take a seed colour from it | Claude spec, agy | Host test for the seed colour and the BMP fallback path; a module without `main.bmp` gets a generated gradient |
| P12.3 | Home and Modules screens in Compose (hero, Play, status chip, card grid, shared-element transition) | agy | Screenshot tests pass in CI |
| P12.4 | Settings and Advanced screens in Compose; remove the old view-based pages and `LauncherUi` | agy | Every P10 setting and developer control is reachable; screenshot tests pass |
| P12.5 | **Screenshot tests in CI** (Roborazzi on Robolectric, test-only dependencies): Home, Modules, Settings in portrait and landscape, light and dark, with placeholder art; images uploaded as a CI artifact | Claude spec, agy | The artifact shows every screen, so the look can be reviewed without a phone |

**Batches.** One batch: P12.1 first (others build on it), then P12.2–P12.5 in parallel
worktrees. One gate, one push, one APK, plus the screenshot artifact for review.

**Status (2026-11-18).** P12.1 landed: Compose BOM `2024.10.01` + Material 3, Kotlin Compose
plugin `2.0.21`, `android.useAndroidX=true`; `LauncherViewModel` owns the P10 launcher state
and JNI calls; `Wse2Theme` (dynamic colour on Android 12+, `schemeFromSeed` fallback);
interim Compose screens (`LauncherApp`, `InterimScreens`) on a `NavigationBar` /
`NavigationRail`. The view-based pages stay compiled because the ViewModel reuses
`InstallCheckState` and `PerformanceUiState`; P12.4 removes them. Only CI compiles it (no SDK on
the host). Wave B added P12.2 (`wse/asset/bmp.h`: `decode_bmp`, `downscale_to_fit`,
`seed_color_argb`; JNI `nativeLoadModuleArt`) and P12.3/P12.4 (Compose `HomeScreen`,
`ModulesScreen`, `SettingsScreen`, `AdvancedScreen` taking plain state, module art with an LRU
cache and a seed colour, shared-element transition, predictive back; the view-based pages,
`LauncherUi` and `themes.xml` removed). Left: P12.5 screenshot tests.

## P13 — Launcher art direction: modern medieval

**Why.** The P12 launcher works on the user's phone (2026-11-18) but "still looks kinda flat and
soulless". Measured in `android/.../launcher/` (5,253 lines of Kotlin): every surface is a stock
Material 3 component (9 `OutlinedButton`, 6 `TextButton`, 2 `Button`, 2 `FilledTonalButton`,
2 `AssistChip`, 7 `ListItem`, 6 `Surface`, 3 `AlertDialog`, stock `NavigationBar` /
`NavigationRail`), all type is `FontFamily.SansSerif`, backgrounds are flat scheme colours,
and the colours come from the wallpaper (`dynamicDarkColorScheme`).

**Decisions (interview, 2026-11-18).**
- *Direction:* medieval and cinematic, but modern: a current game store page for a medieval
  game, not a fantasy skin. Detailing stays **subtle**: chamfered corners, thin gold hairline
  borders, a serif display face for titles, film grain and vignette; clean sans body text.
  No ornate flourishes or engraved textures.
- *What reads as flat:* stock components, generic type, plain backgrounds. Motion: **moderate**
  (smooth transitions, press feedback, a gentle hero pan; no ambient particles).
- *Colour:* a **fixed dark iron-and-gold palette**; the selected module's art seed colour tints
  the accents and glow. Wallpaper (dynamic) colour is removed.
- *Theme:* **dark only**; the light scheme is removed.
- *Fonts:* bundle open-licence (SIL OFL) fonts in `res/font/` with their licence text: a serif
  display face (Cinzel or similar) for titles and a sans for body text. Fonts are not game
  assets, so BYOA holds; sizes are measured before choosing and recorded here.

| Unit | What | Owner | Done when |
| --- | --- | --- | --- |
| P13.0 | **Screenshot tests in CI** (was P12.5): Roborazzi on Robolectric, test-only dependencies; `HomeScreen`, `ModulesScreen`, `SettingsScreen`, `AdvancedScreen` with fake state and generated placeholder art, portrait and landscape, dark; PNGs uploaded as a CI artifact | Claude spec, agy | The artifact shows every screen; Claude reviews the look from it before each launcher APK |
| P13.1 | Design tokens: fonts, the iron-and-gold scheme with art-seed accent tint, a chamfered shape set, and a backdrop modifier (gradient + grain + vignette, drawn in code) | Claude spec, agy | `Wse2Theme` has no dynamic or light scheme; type uses the bundled fonts; screenshots show the backdrop |
| P13.2 | Component kit replacing the stock widgets: primary Play button (gold, chamfered, sheen on press), secondary and text buttons with hairline borders, status chip, framed art banner card with a nameplate, section header with a hairline divider, list rows, themed switch / slider / segmented control / dialog / navigation bar and rail | Claude spec, agy | No stock Material button, chip, `ListItem`, `AlertDialog` or navigation component is used directly in the four screens (grep check); screenshots reviewed |
| P13.3 | Screens restyled on the kit, plus moderate motion: slow hero pan (Ken Burns), press scale, staggered card entrance, existing fade-through; all animations follow the system animator scale (off when the user disables animations) | agy | Screenshots reviewed by Claude and the user's phone check; APK growth stays within the font sizes plus 100 KB |

**Batches.** P13.0 and P13.1+P13.2 in parallel (different files), then P13.3. One gate, one
push, one APK per batch; every launcher APK ships with its screenshot artifact.

**Status (2026-11-18).** P13.0-P13.2 landed and CI renders 12 screenshots (`launcher-screenshots`
artifact; Roborazzi `1.36.0`, Robolectric `4.14.1`, SDK 34). Fonts: Cinzel `125,468 B` and
Manrope `164,700 B` (variable, SIL OFL 1.1, licences in `res/raw/` and an Advanced "Licences"
row). Contrast checked: body text 15.66:1 on the background, Play label 7.61:1 (5.72:1 on the
gradient's dark end). Claude's screenshot review left these for P13.3:
1. Label weights render thin (variable-font weight possibly not applied).
2. The Play label is too small for the main action.
3. The hero's blur also blurs its frame lines.
4. Empty space below the Home actions (portrait, and the landscape column).
5. The slider keeps stock tick dots, and the switches are stock pills.
6. The selected card's glow is stepped.
7. The tests render screens without the app backdrop.
8. The hero blur is too strong for real art.

P13.3 landed (Claude, after agy's quota ran out), with the user's review on screenshots:
- Manrope: the variable file defaults to weight 200 and lost its per-weight variations, so
  every label rendered near ExtraLight. It is now three static files
  (`manrope_{medium,semibold,bold}.ttf`, 144,696 / 145,104 / 146,104 B); Cinzel stays variable.
- Home, portrait: the user chose "taller hero + module strip". The hero takes 55 % of the
  height, Play is 60 dp, a "Your modules" strip of 140x92 dp art cards follows, and the
  secondary buttons sit at the bottom.
- Home, landscape: the hero takes 58 % of the width, with a 2-column module grid.
- Hero: the scrim ends opaque (no bright band at the bottom), and the art is clipped with a
  light 3 dp blur.
- Kit: a chamfered `WseSwitch`, a tick-free diamond-thumb `WseSlider`, and a 12-ring
  selected glow.
- Motion: an 18 s Ken Burns drift on the hero and a staggered fade-up of the Home blocks,
  both off at animator scale 0 and in tests (`LocalInspectionMode`).
- Screenshots: rendered on the app backdrop.

P13.3 follow-up (agy, reviewed and fixed by Claude, 2026-11-21; CI screenshots, no phone check
yet):
- Modules: staggered entrance on the header, cards and details; the "ACTIVE" label is a badge
  on the card art, so it no longer clips on missing-files cards.
- Motion: tab fade-through, press scale and press sheen are off at animator scale 0 and in tests.
- Home: the hero fades into the backdrop through an alpha mask (no seam), with a crest pill;
  `ModuleArtGraphic` uses unbounded blur behind an opaque dark offscreen base and 6% overscan,
  removing the thin bright line along the hero's top edge in portrait and landscape.
- Settings and Advanced: editorial headers; three long Advanced button labels shortened.
- Settings edits `rgl_config.txt` (`RglConfig.kt`): every key of a Warband config, grouped in six
  categories, unknown keys kept as written, and the quality preset, frame-rate cap and present
  mode mirrored into it. The file is the one the R4 run's exe reads (`filesDir/recomp/r4/user/
  Documents/Mount&Blade Warband/`). Opening the app writes nothing: with no file the exe makes its
  own, as on Windows, and the screen shows defaults. The first change writes it; a change made
  before the file was read applies to the file's contents, never to defaults. A file that cannot
  be read is moved to `rgl_config.txt.unreadable` before a write, never overwritten.

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
