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

# Ensure stock MIT Lua 5.1.5 is cached and verified under engine/build/third_party/.
$thirdParty = Join-Path $here "build\third_party"
$luaRoot = Join-Path $thirdParty "lua-5.1.5"
$luaSrc = Join-Path $luaRoot "src"
$luaTar = Join-Path $thirdParty "lua-5.1.5.tar.gz"
$luaExpectedHash = "2640FC56A795F29D28EF15E13C34A47E223960B0240E8CB0A82D9B0738695333"
if (-not (Test-Path (Join-Path $luaSrc "lua.h"))) {
    New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null
    if (-not (Test-Path $luaTar)) {
        Write-Host "downloading  lua-5.1.5.tar.gz"
        Invoke-WebRequest -Uri "https://www.lua.org/ftp/lua-5.1.5.tar.gz" -OutFile $luaTar -UseBasicParsing
    }
    $actualHash = (Get-FileHash -Path $luaTar -Algorithm SHA256).Hash
    if ($actualHash -ne $luaExpectedHash) {
        throw "lua-5.1.5.tar.gz SHA-256 mismatch: expected $luaExpectedHash, got $actualHash"
    }
    & tar -xf $luaTar -C $thirdParty
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $luaSrc "lua.h"))) {
        throw "failed to extract lua-5.1.5.tar.gz"
    }
}

$luaCoreFiles = @(
    "lapi.c", "lauxlib.c", "lbaselib.c", "lcode.c", "ldblib.c", "ldebug.c",
    "ldo.c", "ldump.c", "lfunc.c", "lgc.c", "linit.c", "liolib.c", "llex.c",
    "lmathlib.c", "lmem.c", "loadlib.c", "lobject.c", "lopcodes.c", "loslib.c",
    "lparser.c", "lstate.c", "lstring.c", "lstrlib.c", "ltable.c", "ltablib.c",
    "ltm.c", "lundump.c", "lvm.c", "lzio.c"
)
$luaObjDir = Join-Path $out "lua51_obj"
New-Item -ItemType Directory -Force -Path $luaObjDir | Out-Null
$luaLib = Join-Path $out "liblua51.a"
if (-not (Test-Path $luaLib)) {
    Write-Host "building     lua51"
    $luaObjs = @()
    foreach ($cf in $luaCoreFiles) {
        $obj = Join-Path $luaObjDir ($cf -replace '\.c$', '.o')
        & $zig cc -target x86_64-windows-gnu -std=c99 -O2 -w -I $luaSrc -c (Join-Path $luaSrc $cf) -o $obj
        if ($LASTEXITCODE -ne 0) { throw "lua51 compile failed: $cf" }
        $luaObjs += $obj
    }
    & $zig ar rcs $luaLib @luaObjs
    if ($LASTEXITCODE -ne 0) { throw "lua51 archive failed" }
}

$wseLuaFiles = @(
    "lua_bitop.cpp", "wse_lua_callbacks.cpp", "wse_lua_manager.cpp", "wse_lua_ops.cpp"
)
$wseLuaObjDir = Join-Path $out "wse_lua_obj"
New-Item -ItemType Directory -Force -Path $wseLuaObjDir | Out-Null
$wseLuaLib = Join-Path $out "libwse_lua.a"
if (Test-Path $wseLuaLib) { Remove-Item -Force $wseLuaLib }
Write-Host "building     wse_lua"
$wseLuaObjs = @()
foreach ($cpp in $wseLuaFiles) {
    $obj = Join-Path $wseLuaObjDir ($cpp -replace '\.cpp$', '.o')
    & $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror `
        -I $include -I $luaSrc -c (Join-Path $here "src\lua\$cpp") -o $obj
    if ($LASTEXITCODE -ne 0) { throw "wse_lua compile failed: $cpp" }
    $wseLuaObjs += $obj
}
& $zig ar rcs $wseLuaLib @wseLuaObjs
if ($LASTEXITCODE -ne 0) { throw "wse_lua archive failed" }

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
    @{ Name = "block_vm"; Source = Join-Path $here "tests\block_vm_test.cpp" },
    @{ Name = "native_world"; Source = Join-Path $here "tests\native_world_test.cpp" },
    @{ Name = "native_m5"; Source = Join-Path $here "tests\native_m5_test.cpp" },
    @{ Name = "wse_m6"; Source = Join-Path $here "tests\wse_m6_test.cpp" },
    @{ Name = "platform"; Source = Join-Path $here "tests\platform_test.cpp" },
    @{ Name = "module_loaders"; Source = Join-Path $here "tests\module_loaders_test.cpp" },
    @{ Name = "headless"; Source = Join-Path $here "tests\headless_test.cpp" },
    @{ Name = "lua"; Source = Join-Path $here "tests\lua_test.cpp"; LinkLua = $true }
)

Write-Host "zig          $zig"
Write-Host "output       $out"

foreach ($test in $tests) {
    $binary = Join-Path $out ("{0}_test.exe" -f $test.Name)
    Write-Host ("building     {0}" -f $test.Name)
    $extraLibs = if ($test.ContainsKey("LinkLua") -and $test.LinkLua) { @($wseLuaLib, $luaLib) } else { @() }
    & $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror `
        -I $include -o $binary $test.Source @extraLibs
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

$selftestObjDir = Join-Path $out "selftest_obj"
New-Item -ItemType Directory -Force -Path $selftestObjDir | Out-Null
$selftestSources = @($tests | ForEach-Object { $_.Source }) + @(Join-Path $here "tests\wse_selftest.cpp")
$selftestObjs = @()
Write-Host "building     selftest_host"
foreach ($src in $selftestSources) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($src)
    $obj = Join-Path $selftestObjDir ("{0}.o" -f $stem)
    & $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror `
        -DWSE_SELF_TEST_LIBRARY=1 -I $include -c $src -o $obj
    if ($LASTEXITCODE -ne 0) {
        throw ("selftest compile failed: {0}" -f $src)
    }
    $selftestObjs += $obj
}
$selftestBinary = Join-Path $out "selftest_host_test.exe"
& $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror `
    -I $include -o $selftestBinary (Join-Path $here "tests\selftest_host_test.cpp") `
    @selftestObjs $wseLuaLib $luaLib
if ($LASTEXITCODE -ne 0) {
    throw "selftest_host link failed"
}
& $selftestBinary (Join-Path $here "fixtures\scripts.txt")
if ($LASTEXITCODE -ne 0) {
    throw "selftest_host tests failed"
}

$headlessTool = Join-Path $out "wse_headless.exe"
Write-Host "building     wse_headless"
& $zig c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wall -Wextra -Werror `
    -I $include -o $headlessTool (Join-Path $here "tools\wse_headless.cpp") $wseLuaLib $luaLib
if ($LASTEXITCODE -ne 0) {
    throw "wse_headless build failed"
}

Write-Host "engine host tests: PASS"
