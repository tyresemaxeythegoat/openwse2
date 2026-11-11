#!/usr/bin/env pwsh
# Host-side differential test for the native PanVK loader.
#
# Builds vk_panvk_loader.c + panvk_probe.c against a mock ICD for the host
# architecture, then runs the probe. No GPU, phone, or Android SDK is needed.
#
# Usage:
#   pwsh runtime/android/build_host.ps1 [-Zig C:\path\to\zig.exe]

[CmdletBinding()]
param(
    [string]$Zig
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = Join-Path $here "build\host"
New-Item -ItemType Directory -Force -Path $out | Out-Null

function Resolve-Zig([string]$explicit) {
    if ($explicit) {
        if (-not (Test-Path $explicit)) {
            throw "zig not found at $explicit"
        }
        return (Resolve-Path $explicit).Path
    }

    $command = Get-Command zig -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $bundled = Join-Path $env:APPDATA "Python\Python312\site-packages\ziglang\zig.exe"
    if (Test-Path $bundled) {
        return $bundled
    }

    throw "zig not found; pass -Zig or put zig on PATH"
}

$zig = Resolve-Zig $Zig
$include = @("-I", (Join-Path $here "third_party"), "-I", (Join-Path $here "src"))

$probe = Join-Path $out "probe_host.exe"
$mock = Join-Path $out "mock_icd.dll"

Write-Host "zig          $zig"
Write-Host "output       $out"

& $zig cc -target x86_64-windows-gnu -O2 `
    -DWSE2_PANVK_SINGLE_THREADED @include `
    -o $probe `
    (Join-Path $here "src\vk_panvk_loader.c") `
    (Join-Path $here "src\panvk_probe.c")
if ($LASTEXITCODE -ne 0) {
    throw "host probe build failed"
}

& $zig cc -target x86_64-windows-gnu -O2 -shared @include `
    -o $mock `
    (Join-Path $here "tests\mock_icd.c")
if ($LASTEXITCODE -ne 0) {
    throw "mock ICD build failed"
}

Write-Host ""
& $probe $mock
if ($LASTEXITCODE -ne 0) {
    throw "host probe failed with exit code $LASTEXITCODE"
}

Write-Host ""
Write-Host "host loader test: PASS"
