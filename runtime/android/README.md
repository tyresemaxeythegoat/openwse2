# Native Android / PanVK path

This directory connects the AArch64 recompiled engine to **PanVK**, Mesa's
native Vulkan driver for Mali kbase GPUs. It is not Wine, not Winlator, not
Box64, and not an x86 emulator. The only translation layer in the graphics
stack is D3D9 -> Vulkan (dxvk-native), which is an API translation, not CPU or
GPU emulation.

Target device for the validation notes below: OPPO Reno 11, Mali-G68 (Valhall
v9, kbase). PanVK 3.7.0 lists G68 among its supported targets.

## Why the app loads the ICD itself

Android's platform `libvulkan.so` discovers drivers through
`hw_get_module("vulkan")` and the vendor namespace. On a retail device it does
not honour `VK_ICD_FILENAMES`, and an ordinary app cannot register a custom
ICD with the platform loader. The app therefore bypasses the platform loader
and talks to the ICD through the documented Vulkan ICD interface:

```
vk_icdNegotiateLoaderICDInterfaceVersion
vk_icdGetInstanceProcAddr
vk_icdGetPhysicalDeviceProcAddr
```

`src/vk_panvk_loader.c` is a *loader*, not a driver: it `dlopen`s
`libvulkan_panfrost.so`, negotiates loader interface version 5, and forwards
global/instance/physical-device/device commands to the ICD. The ICD then
submits native jobs to the Mali kbase kernel driver via `/dev/mali0`.

## Why libhardware is replaced

The ICD has these `DT_NEEDED` entries:

```
libhardware.so liblog.so libnativewindow.so libsync.so libm.so libdl.so libc.so
```

`libhardware.so` is a VNDK-SP library and is not reachable from a normal app's
linker namespace. PanVK calls `hw_get_module("gralloc")` only for optional
gralloc/video-buffer support. Shipping a tiny `libpanvk_hw.so` that returns
`-ENOENT` sends PanVK down its supported "no gralloc hwmodule" fallback; normal
swapchain rendering does not use the vendor gralloc HAL. All remaining
dependencies are public NDK libraries.

`tools/patch_panvk.py` rewrites the 14-byte `libhardware.so` string to the
equally long `libpanvk_hw.so` directly in `.dynstr`. File layout, offsets and
hashes are unchanged; only the name is edited.

## Layout

| Path | Role |
|---|---|
| `src/vk_panvk_loader.c` | direct ICD loader/dispatch shim |
| `src/vk_panvk_loader.h` | load/status interface used by the app |
| `src/hardware_stub.c` | freestanding `libpanvk_hw.so` (`hw_get_module` -> `-ENOENT`) |
| `src/panvk_probe.c` | instance/physical-device/logical-device smoke test |
| `tests/mock_icd.c` | host-only mock ICD for the differential test |
| `third_party/` | vendored Khronos Vulkan C headers (Apache-2.0 OR MIT) |
| `build_host.ps1` | build and run the host differential test (Zig, no NDK) |
| `build_android.ps1` | NDK build for `arm64-v8a` |
| `build_android.sh` | same, for POSIX hosts |

## Host differential test

This proves the loader logic and dispatch forwarding without a phone:

```powershell
pwsh runtime/android/build_host.ps1
```

It builds `probe_host.exe` and `mock_icd.dll` under
`runtime/android/build/host/` and runs the probe. Expected tail:

```
ICD            ...\mock_icd.dll
loader/ICD     interface version 5
instance API   1.3.0
physical devs  1
device[0]      Mock Mali-G68 (PanVK ICD)
logical device created on queue family 0
PASS
```

## Android build

Requires an Android NDK (r26+), used only as a compiler sysroot. Nothing is
installed system-wide.

```powershell
pwsh runtime/android/build_android.ps1 -Ndk C:\Android\Sdk\ndk\27.0.12077973
```

or:

```sh
ANDROID_NDK_HOME=/opt/android-ndk ./runtime/android/build_android.sh
```

Outputs in `build/android/arm64-v8a/`:

- `libpanvk_hw.so` — freestanding stub, no `DT_NEEDED`
- `libwse2_panvk.so` — the loader, for the runtime to link/load
- `panvk_probe` — executable smoke test for `adb shell`

## Patch and verify the ICD

The PanVK package is not redistributed here. Supply your own copy of the
extracted `libvulkan_panfrost.so`:

```powershell
python tools/patch_panvk.py <PanVK>\libvulkan_panfrost.so `
    -o runtime/android/build/android/arm64-v8a/libvulkan_panfrost.so

python tools/check_panvk.py `
    runtime/android/build/android/arm64-v8a/libvulkan_panfrost.so `
    --stub runtime/android/build/android/arm64-v8a/libpanvk_hw.so
```

`check_panvk.py` verifies ELF64/AArch64/ET_DYN, SONAME, the three ICD exports,
the patched NEEDED list, the presence of the `hw_get_module` import, and the
stub's own SONAME/exports/no-dependencies. It exits non-zero on any mismatch.

## Packaging into the APK

1. Put `libvulkan_panfrost.so`, `libpanvk_hw.so` and (optionally)
   `libwse2_panvk.so` in `app/src/main/jniLibs/arm64-v8a/`.
2. Set `android:extractNativeLibs="true"` on `<application>` (or
   `packaging { jniLibs { useLegacyPackaging = true } }` in Gradle). The loader
   needs real files in `ApplicationInfo.nativeLibraryDir`; with the modern
   default the `.so` may live only inside the APK and an absolute-path
   `dlopen` fails.
3. Pass the absolute path to the loader:

   ```c
   const char *dir = /* ApplicationInfo.nativeLibraryDir from JNI */;
   snprintf(path, sizeof(path), "%s/libvulkan_panfrost.so", dir);
   wse2_panvk_load(path);
   ```

   The default name `libvulkan_panfrost.so` also resolves from the app's
   native-library namespace, but the absolute path is unambiguous.

4. The ICD's `DT_NEEDED` entries `liblog`, `libnativewindow`, `libsync`,
   `libm`, `libdl`, `libc` are all in `public.libraries.txt`; `libpanvk_hw.so`
   resolves from the same directory. No VNDK access is required.

## On-device probe

```sh
adb push runtime/android/build/android/arm64-v8a/panvk_probe /data/local/tmp/
adb push runtime/android/build/android/arm64-v8a/libpanvk_hw.so /data/local/tmp/
adb push runtime/android/build/android/arm64-v8a/libvulkan_panfrost.so /data/local/tmp/
adb shell chmod 755 /data/local/tmp/panvk_probe
adb shell /data/local/tmp/panvk_probe /data/local/tmp/libvulkan_panfrost.so
```

Expected on a G68:

```
ICD            /data/local/tmp/libvulkan_panfrost.so
loader/ICD     interface version 5
instance API   1.3.x
device[0]      Mali-G68 ...
  queue[0]     flags 0x... count ...
logical device created on queue family 0
PASS
```

If `vkCreateInstance` or `vkCreateDevice` fails, check:

- `adb shell ls -l /dev/mali0` and that the app process can open it;
- `adb logcat -s panvk-loader` and the Mesa/PanVK log lines;
- that the ICD was patched (`check_panvk.py` must pass);
- that `libpanvk_hw.so` sits next to the ICD;
- that the APK extracts native libraries (`extractNativeLibs`).

A log line containing `No gralloc hwmodule detected (video buffers won't be
supported)` is expected and is not an error for normal rendering.

## What is still missing

- The probe validates instance/device creation, not a swapchain. The next step
  is `ANativeWindow` -> `vkCreateAndroidSurfaceKHR` -> swapchain -> clear/present.
- dxvk-native's WSI currently creates surfaces through SDL2/the platform
  loader. The runtime must either hand DXVK an `ANativeWindow`-backed
  `VkSurfaceKHR` created through this loader, or patch the WSI to call
  `vkCreateAndroidSurfaceKHR` directly. Otherwise DXVK may bind to the system
  `libvulkan.so` and bypass PanVK.
- Real hardware validation is blocked in this workspace: there is no Android
  device, emulator, `adb`, or NDK. Everything above is static/host-differential
  until the probe is run on the phone.
- The D3DX9 effect VM, the FMOD Ex shim, and the full engine lift are separate
  work items.

## Legal

Do not commit the PanVK `.so`, the game executable, or game data to this
repository or place them in a public APK. PanVK is Mesa (MIT); the vendored
Vulkan headers are Apache-2.0 OR MIT.
