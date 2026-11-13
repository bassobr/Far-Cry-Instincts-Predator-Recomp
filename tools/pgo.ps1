# PGO of the recompiled code: instrumented build, training runs,
# merge, optimized build. The profile is derived from the game and stays under out\pgo (never in the repository).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pgo.ps1 [-Config release] [-BuildCmd build.cmd]
#       [-Levels 21_dunea,06_cliffs,26_shantynight] [-Seconds 55] [-Off]
# In a git worktree: -BuildCmd tools\boot\wt_build.cmd. -Off turns PGO off again in the build folder (FCP_PGO empty) and builds.
# A stale profile (after a codegen, hook or compiler change) is only slower, never wrong; clang does not report
# missing/mismatching functions (warnings off), so retrain after such changes.
param(
  [string]$Config = "release",
  [string]$BuildCmd = "build.cmd",
  [string[]]$Levels = @("21_dunea", "06_cliffs", "26_shantynight"),
  [int]$Seconds = 55,
  [switch]$Off
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bdir = Join-Path $root "out\build\$Config"
$pgo = Join-Path $root "out\pgo"
$raw = Join-Path $pgo "raw"
$toolchain = Join-Path $root "toolchain"
$tc = Select-String (Join-Path $bdir "CMakeCache.txt") -Pattern "^FCP_TOOLCHAIN_DIR:PATH=(.*)$" -ErrorAction SilentlyContinue
if ($tc) { $toolchain = $tc.Matches[0].Groups[1].Value }

function SetPgo([string]$value) {
  # Only change the entry in the CMake cache; the build then reconfigures by itself (build.ninja depends on the cache)
  # with the environment of the build script. A separate cmake call without this environment would reset the FCP_* paths.
  $cache = Join-Path $bdir "CMakeCache.txt"
  $lines = Get-Content $cache
  $found = $false
  $lines = $lines | ForEach-Object { if ($_ -match '^FCP_PGO:STRING=') { $found = $true; "FCP_PGO:STRING=$value" } else { $_ } }
  if (-not $found) { $lines += "FCP_PGO:STRING=$value" }
  Set-Content -Encoding ascii $cache $lines
}
function Build() {
  Push-Location $root
  try { cmd /c "$BuildCmd $Config" } finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw "build failed ($BuildCmd $Config)" }
}

$Levels = @($Levels | ForEach-Object { $_ -split "," } | Where-Object { $_ })
if (-not (Test-Path (Join-Path $bdir "CMakeCache.txt"))) { throw "build folder missing: $bdir (do a normal build first)" }
if ($Off) { SetPgo ""; Build; Write-Output "pgo: off"; exit 0 }

Write-Output "pgo: instrumented build"
SetPgo "generate"; Build
New-Item -ItemType Directory -Force $raw | Out-Null
Get-ChildItem $raw -Filter *.profraw -ErrorAction SilentlyContinue | Remove-Item
foreach ($lv in $Levels) {
  Write-Output "pgo: training $lv ($Seconds s)"
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run.ps1") -Config $Config `
    -Seconds $Seconds -Env "FCP_FPS=120;FCP_START_LEVEL=$lv;FCP_INPUT_SCRIPT_CLOCK=level;FCP_INPUT_SCRIPT=10-50:lup;LLVM_PROFILE_FILE=$raw\$lv-%p.profraw" | Out-Null
}
$files = @(Get-ChildItem $raw -Filter *.profraw | ForEach-Object FullName)
if ($files.Count -ne $Levels.Count) { throw "pgo: $($files.Count) profiles for $($Levels.Count) runs" }
& (Join-Path $toolchain "llvm\bin\llvm-profdata.exe") merge -o (Join-Path $pgo "predator.profdata") $files
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed" }
Write-Output "pgo: optimized build with $(Join-Path $pgo 'predator.profdata')"
SetPgo "use"; Build
Write-Output "pgo: done"
