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
engine/include/wse/platform/   vfs.h saf_vfs.h clock.h sleep.h log.h rhi.h (present) · audio.h input.h (planned)
engine/include/wse/asset/      brf.h, dds.h (portable, host-tested); shader, sound decoders (planned)
platform/sdl/                  SDL3 on-device self-test shell (present); window / input / audio / clock (planned)
platform/vulkan/               driver_loader.{h,cpp}: Vulkan loader, MockVulkanIcd v5, VulkanRhiDevice (present)
platform/android/              saf_vfs_jni.cpp over JNI (present); thermal and frame-rate hooks (planned)
android/                       Gradle project: Kotlin WseLauncherActivity (SAF + module/driver picker) + WseSafBridge + WseActivity (SDLActivity) + Prefab SDL3 arm64-v8a APK (present)
third_party/                   SDL3 (Gradle Prefab), Lua 5.1 (CMake FetchContent), Bullet, volk/VMA — pinned by hash
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
| P4 *(host verified; CI & device pending)* | Vulkan bring-up + driver loader | Both driver packages inspected (`v3.7.0` PanVK + `v27.0.0-20261115` Turnip). `engine/include/wse/platform/rhi.h` (`RhiDevice`, `SoftwareRhiDevice`, `meta.json` parser, ELF `.dynstr` `libpanvk_hw.so` patcher, BRF/DDS textured mesh helper), `platform/vulkan/driver_loader.{h,cpp}` (`VulkanDriverLoader`, `MockVulkanIcd` v5, `VulkanRhiDevice`), `libpanvk_hw.so` (`runtime/android/src/hardware_stub.c`), and `nativeProbeVulkanAndRenderMesh` JNI/launcher probe. | Textured static mesh screenshot test (`SoftwareRhiDevice` vs `VulkanRhiDevice` over `MockVulkanIcd` v5 + `load_system` probe) PASS on host; on-device run with real drivers pending hardware |
| P5 | Shaders + materials | Evidence: how v1.1.5.5 loads module effect files (`d3dx9_43` call sites) and which techniques materials name. Pipeline from the module's shaders to SPIR-V, run once at module import and cached. The translator is chosen by a spike on a real `Native` plus one large mod, not in advance. D3D9 conventions (half-pixel offset, clip-space depth, sRGB) are handled in the RHI. | `Native` core techniques render the viewer mesh correctly; second launch hits the cache |
| P6 | Audio | Evidence: FMOD Ex call sites → required feature set. `AudioDevice` interface; SDL3 audio-stream backend with a small mixer (3D pan and attenuation, streamed music); decoders for the formats `sounds.txt` references. | Host test mixes deterministically to a buffer; device plays a module sound and a music track |
| P7 | Input + touch overlay *(Phase 4)* | `Input` interface mapping SDL3 keyboard, mouse, gamepad, and touch onto the engine's game-key table (also needed by the WSE input-key family). Overlay: movement stick, look area, four-direction attack/block swipe zones, command radial. One `InputContext` (mission / map / menu / inventory / presentation) drives visibility; overlay hides when a gamepad connects. Layout editor, per-module profiles. | Overlay drives a free camera in the P4 viewer; hides in menu context and on controller attach |
| P8 | Sensors + performance *(Phase 4)* | Opt-in gyro aim (filtered), pinch gesture → map zoom, thermal headroom → resolution / LOD bias, display frame-rate request and present mode for 60/90/120 Hz caps. | Each has a settings toggle; frame-rate cap verified on device |
| P9 | Convergence with Phase 2 | Scene, terrain, and skeletal rendering; widgets and presentations; Bullet; Lua 5.1 link (2F); the 2H headless `Native` run on device. Unblocks the engine-backed WSE families (scene, overlays, input keys, reload/material). | `Native` main menu renders on device from a user-supplied install |

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
| Modules may ship shader source, compiled effects, or both | P5 begins with an evidence spike and a measurement of real modules before choosing a translator |
| Mobile GPUs may not sample the block-compressed formats in `.dds` | Query format support; transcode at import and cache on disk |
| No device is attached to any build machine | P0–P3 are fully CI-verifiable; from P2/P4 on, device results come from running the CI APK and reading its self-test screen |
| Third-party licences | Permissive libraries are linked statically; any LGPL component ships only as a separate shared library; no FMOD, no Steam |
