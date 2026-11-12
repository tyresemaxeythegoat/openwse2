#!/usr/bin/env pwsh
# Host tests for the portable engine sources.
#
# Builds the statement/operand tests with Zig's clang++ for the host and runs
# them. The Android cross-compile gate still needs an NDK; this script is the
# host half of the gate.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1 [-Zig C:\path\to\zig.exe]

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
$include = Join-Path $here "include"
$test = Join-Path $out "operand_test.exe"

Write-Host "zig          $zig"
Write-Host "output       $out"

& $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror `
    -I $include -o $test (Join-Path $here "tests\operand_test.cpp")
if ($LASTEXITCODE -ne 0) {
    throw "operand test build failed"
}

& $test
if ($LASTEXITCODE -ne 0) {
    throw "operand tests failed"
}

Write-Host "engine host tests: PASS"
