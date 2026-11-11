#!/usr/bin/env pwsh
# Build the native PanVK loader, the libhardware stub, and the on-device probe.
#
# Requires an Android NDK (r26 or newer). The NDK is only used as a compiler
# sysroot; nothing is installed system-wide. Set ANDROID_NDK_HOME or pass -Ndk.
#
# Usage:
#   pwsh runtime/android/build_android.ps1 -Ndk C:\Android\Sdk\ndk\27.0.12077973
#   pwsh runtime/android/build_android.ps1 -Api 29

[CmdletBinding()]
param(
    [string]$Ndk,
    [string]$Api = "28"
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here "build\android\arm64-v8a"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Find-Ndk([string]$explicit) {
    $candidates = @()
    if ($explicit) { $candidates += $explicit }
    if ($env:ANDROID_NDK_HOME) { $candidates += $env:ANDROID_NDK_HOME }
    if ($env:ANDROID_NDK_ROOT) { $candidates += $env:ANDROID_NDK_ROOT }
    if ($env:NDK_HOME) { $candidates += $env:NDK_HOME }

    $localProperties = Join-Path $here "..\..\local.properties"
    if (Test-Path $localProperties) {
        foreach ($line in Get-Content $localProperties) {
            if ($line -match '^\s*ndk\.dir\s*=\s*(.+)$') {
                $candidates += $Matches[1].Trim()
            }
        }
    }

    $sdkRoots = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT,
        (Join-Path $env:LOCALAPPDATA "Android\Sdk"),
        (Join-Path $HOME "Android\Sdk"),
        (Join-Path $HOME "Library\Android\sdk")) |
        Where-Object { $_ -and (Test-Path $_) }

    foreach ($sdk in $sdkRoots) {
        $ndkDir = Join-Path $sdk "ndk"
        if (Test-Path $ndkDir) {
            $candidates += (Get-ChildItem $ndkDir -Directory |
                Sort-Object Name -Descending |
                Select-Object -ExpandProperty FullName)
        }
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "toolchains\llvm"))) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw @"
Android NDK not found.
Pass -Ndk, set ANDROID_NDK_HOME, or install the NDK under the SDK's ndk/ folder.
Download: https://developer.android.com/ndk/downloads
"@
}

function Get-HostTag {
    if ($env:OS -eq "Windows_NT") { return "windows-x86_64" }
    if ($env:OSTYPE -match "darwin" -or (Test-Path "/System/Library")) { return "darwin-x86_64" }
    return "linux-x86_64"
}

$ndkPath = Find-Ndk $Ndk
$hostTag = Get-HostTag
$binDir = Join-Path $ndkPath "toolchains\llvm\prebuilt\$hostTag\bin"
$clangName = "aarch64-linux-android$Api-clang"
if ($env:OS -eq "Windows_NT") { $clangName += ".cmd" }
$cc = Join-Path $binDir $clangName

if (-not (Test-Path $cc)) {
    throw "compiler not found: $cc"
}

$include = @("-I$(Join-Path $here 'third_party')", "-I$(Join-Path $here 'src')")
$stub = Join-Path $out "libpanvk_hw.so"
$probe = Join-Path $out "panvk_probe"
$loader = Join-Path $out "libwse2_panvk.so"

Write-Host "NDK          $ndkPath"
Write-Host "compiler     $cc"
Write-Host "API          $Api"
Write-Host "output       $out"
Write-Host ""

# The stub is freestanding: no crt, no libc, no DT_NEEDED entries.
& $cc -O2 -fPIC -shared -nostdlib "-Wl,-soname,libpanvk_hw.so" `
    -o $stub (Join-Path $here "src\hardware_stub.c")
if ($LASTEXITCODE -ne 0) { throw "stub build failed" }

# Probe executable for adb shell / app-driven smoke tests.
& $cc -O2 -g @include -o $probe `
    (Join-Path $here "src\vk_panvk_loader.c") `
    (Join-Path $here "src\panvk_probe.c") `
    -llog
if ($LASTEXITCODE -ne 0) { throw "probe build failed" }

# Loader as a shared library, for the runtime that will use DXVK's Vulkan path.
& $cc -O2 -g -fPIC -shared @include "-Wl,-soname,libwse2_panvk.so" `
    -o $loader (Join-Path $here "src\vk_panvk_loader.c") -llog
if ($LASTEXITCODE -ne 0) { throw "loader build failed" }

Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize

Write-Host @"
Next steps (see runtime/android/README.md):
  1. python tools/patch_panvk.py <PanVK>/libvulkan_panfrost.so -o build/android/arm64-v8a/libvulkan_panfrost.so
  2. python tools/check_panvk.py build/android/arm64-v8a/libvulkan_panfrost.so --stub build/android/arm64-v8a/libpanvk_hw.so
  3. Package libvulkan_panfrost.so, libpanvk_hw.so and panvk_probe under jniLibs/arm64-v8a/.
"@
