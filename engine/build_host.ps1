#!/usr/bin/env pwsh
# Host tests for the portable engine sources.
#
# Builds every test with Zig's clang++ for the host and runs it. The Android
# cross-compile gate still needs an NDK; this script is the host half of the
# gate.
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

$tests = @(
    @{ Name = "operand"; Source = Join-Path $here "tests\operand_test.cpp" },
    @{ Name = "trigger"; Source = Join-Path $here "tests\trigger_test.cpp" },
    @{ Name = "savegame"; Source = Join-Path $here "tests\savegame_test.cpp" },
    @{ Name = "script"; Source = Join-Path $here "tests\script_test.cpp";
       Args = @(Join-Path $here "fixtures\scripts.txt") },
    @{ Name = "wse_bitwise_fp"; Source = Join-Path $here "tests\wse_bitwise_fp_test.cpp" },
    @{ Name = "wse_system"; Source = Join-Path $here "tests\wse_system_test.cpp" },
    @{ Name = "wse_string"; Source = Join-Path $here "tests\wse_string_test.cpp" },
    @{ Name = "wse_dict"; Source = Join-Path $here "tests\wse_dict_test.cpp" },
    @{ Name = "wse_array"; Source = Join-Path $here "tests\wse_array_test.cpp" },
    @{ Name = "wse_world"; Source = Join-Path $here "tests\wse_world_test.cpp" },
    @{ Name = "block_vm"; Source = Join-Path $here "tests\block_vm_test.cpp" }
)

Write-Host "zig          $zig"
Write-Host "output       $out"

foreach ($test in $tests) {
    $binary = Join-Path $out ("{0}_test.exe" -f $test.Name)
    Write-Host ("building     {0}" -f $test.Name)
    & $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror `
        -I $include -o $binary $test.Source
    if ($LASTEXITCODE -ne 0) {
        throw ("{0} test build failed" -f $test.Name)
    }

    if ($test.ContainsKey("Args")) {
        & $binary @($test.Args)
    } else {
        & $binary
    }
    if ($LASTEXITCODE -ne 0) {
        throw ("{0} tests failed" -f $test.Name)
    }
}

Write-Host "engine host tests: PASS"
