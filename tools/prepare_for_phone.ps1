# Prepares the game for your phone on this PC (much faster than on the phone), 2026-11-21.
#
#   Double-click tools\prepare_for_phone.bat (or: powershell -ExecutionPolicy Bypass -File tools\prepare_for_phone.ps1)
#
# It translates YOUR OWN mb_warband_wse2_x64.exe and lua5164.dll for Android phones and writes
# Downloads\wse2-prepared-for-phone.zip. Copy that zip into the Warband folder on your phone (the
# one you picked in the app); the app's Game test finds it and imports it by itself. The zip is
# made from your copy of the game: keep it for yourself, do not share it.
#
# Needs WSL with Ubuntu (the first run installs the build tools and builds the translator, once).
# The translation itself runs tools/r4_run --for-phone (docs/RECOMP_PLAN.md "Preparing on a PC").

param(
    [string]$Game = "",     # the Warband folder; asked for when not found
    [int]$Cores = 0         # translation threads (default: what WSL's memory allows)
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$zipName = "wse2-prepared-for-phone.zip"

function Say([string]$text) { Write-Host $text }
function Fail([string]$text) {
    Write-Host ""
    Write-Host $text -ForegroundColor Red
    Write-Host ""
    Read-Host "Press Enter to close" | Out-Null
    exit 1
}
function Wsl-Run([string]$script) {
    # Runs a bash script inside WSL as root; returns its exit code (output goes to the screen).
    $tmp = Join-Path $env:TEMP "wse2_prepare_step.sh"
    [System.IO.File]::WriteAllText($tmp, ($script -replace "`r", ""))
    $wslTmp = (& wsl.exe -u root -e wslpath -a "$tmp").Trim()
    & wsl.exe -u root -e bash "$wslTmp"
    return $LASTEXITCODE
}

Say "Preparing Warband (WSE2) for your phone"
Say "========================================"

# 1. WSL
$null = & wsl.exe -u root -e true 2>$null
if ($LASTEXITCODE -ne 0) {
    Fail ("This needs WSL (Windows Subsystem for Linux) with Ubuntu.`n" +
          "Open PowerShell as administrator, run:  wsl --install -d Ubuntu`n" +
          "restart the PC, then run this again.")
}

# 2. The game folder
$candidates = @(
    $Game,
    "C:\GOG Games\Mount and Blade - Warband",
    "C:\Program Files (x86)\Steam\steamapps\common\MountBlade Warband",
    "C:\Program Files\Steam\steamapps\common\MountBlade Warband"
) | Where-Object { $_ -ne "" }
$game = $null
foreach ($c in $candidates) {
    if (Test-Path (Join-Path $c "mb_warband_wse2_x64.exe")) { $game = $c; break }
}
if ($null -eq $game) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Choose your Warband folder (the one with mb_warband_wse2_x64.exe)"
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail "No folder chosen." }
    $game = $dlg.SelectedPath
}
foreach ($f in @("mb_warband_wse2_x64.exe", "mb_warband_wse2_x64.pdb", "lua5164.dll")) {
    if (-not (Test-Path (Join-Path $game $f))) {
        Fail "$f is missing from $game. Choose the Warband folder that has WSE2 installed."
    }
}
Say "Game folder: $game"

$wslRepo = (& wsl.exe -u root -e wslpath -a "$repo").Trim()
$wslGame = (& wsl.exe -u root -e wslpath -a "$game").Trim()
$downloads = Join-Path $env:USERPROFILE "Downloads"
$wslZip = (& wsl.exe -u root -e wslpath -a (Join-Path $downloads $zipName)).Trim()
$jobs = if ($Cores -gt 0) { $Cores } else { 0 }

# 3. Build tools and the translator (first run only; later runs rebuild only what changed)
Say ""
Say "Step 1 of 3: getting the translator ready (the first time takes a while)..."
$code = Wsl-Run @"
set -e
need=""
for p in cmake clang-18 lld-18 llvm-18-dev liblld-18-dev python3; do
  dpkg -s `$p >/dev/null 2>&1 || need="`$need `$p"
done
if [ -n "`$need" ]; then
  echo "Installing build tools:`$need"
  apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq `$need >/dev/null
fi
OUT=/root/wse/build-phone
mkdir -p "`$OUT"
if [ ! -f "`$OUT/CMakeCache.txt" ]; then
  CC=clang-18 CXX=clang++-18 cmake -S "$wslRepo" -B "`$OUT" -DWSE_ENABLE_LLVM=ON -DCMAKE_BUILD_TYPE=Release \
    -DWSE_BUILD_TESTS=OFF -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm -DLLD_DIR=/usr/lib/llvm-18/lib/cmake/lld \
    "-DCMAKE_CXX_FLAGS=-Wno-#warnings" >"`$OUT/configure.log" 2>&1 || { tail -20 "`$OUT/configure.log"; exit 1; }
fi
cmake --build "`$OUT" -j2 --target r4_run >"`$OUT/build.log" 2>&1 || { grep -m20 error "`$OUT/build.log"; exit 1; }
"@
if ($code -ne 0) { Fail "Could not build the translator (details above). Please share this window's text." }

# 4. Translate for the phone
Say ""
Say "Step 2 of 3: preparing the game for your phone (usually under an hour)..."
$code = Wsl-Run @"
set -o pipefail
J=$jobs
if [ "`$J" -le 0 ]; then
  # About 1.5 GB per thread; WSL often has only a few GB.
  J=`$(( `$(awk '/MemAvailable/ {print `$2}' /proc/meminfo) / 1500000 ))
  [ "`$J" -lt 1 ] && J=1
  [ "`$J" -gt `$(nproc) ] && J=`$(nproc)
fi
echo "Using `$J threads."
mkdir -p /root/wse/phone-parts /root/wse/phone-work
/root/wse/build-phone/r4_run "$wslGame" --work /root/wse/phone-work --for-phone /root/wse/phone-parts --jobs "`$J" --quiet 2>&1 \
  | grep --line-buffered -E "batches|batch [0-9]+ done|RESULT|error|cannot|PDB" \
  | sed -u -E 's/^ *(exe|lua5164) batch [0-9]+ done \(([0-9]+)\/([0-9]+), ([0-9]+) s\)/  \1: \2 of \3 parts done (\4 s)/'
"@
if ($code -ne 0) { Fail "Preparing the game did not finish (details above). Run this again: finished parts are kept." }

# 5. Zip
Say ""
Say "Step 3 of 3: packing it into one file..."
$code = Wsl-Run @"
set -e
cd /root/wse/phone-parts
rm -f "$wslZip"
python3 - "$wslZip" <<'PY'
import sys, zipfile, glob
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for f in sorted(glob.glob("exe_w*.so") + glob.glob("exe_w*.key") + glob.glob("lua5164_w*.so") + glob.glob("lua5164_w*.key")):
        z.write(f)
PY
"@
if ($code -ne 0) { Fail "Could not write the zip (details above)." }

$zip = Join-Path $downloads $zipName
$mb = [math]::Round((Get-Item $zip).Length / 1MB)
Say ""
Write-Host "Done! Your phone's game files are ready: $zip ($mb MB)" -ForegroundColor Green
Say ""
Say "Next:"
Say "  1. Copy $zipName into the Warband folder on your phone (the folder you picked in the app)."
Say "     USB cable (choose 'File transfer' on the phone), Quick Share, or Google Drive all work."
Say "  2. In the app, open Advanced and tap Game test. It finds the file and imports it by itself."
Say ""
Read-Host "Press Enter to close" | Out-Null
