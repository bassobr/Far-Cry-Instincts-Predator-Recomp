# PGO of the recompiled code: instrumented training build, training runs, merge, optimized build.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pgo.ps1 [-Config release] [-BuildCmd build.cmd]
#       [-Runs <spec;spec>] [-Seconds 55] [-SkipTrain] [-NoBuild] [-Off]
# Steps:
#  1. tools\pgo_train_build.cmd: out\build\pgo-train, the game code linked into predator.exe and instrumented
#     (-DFCP_GAME_CODE_DLL=OFF -DFCP_PGO=generate; predator_code.dll has no CRT for the profile runtime);
#  2. one run per training spec (developer start of a level with an input script; written to out\pgo\raw);
#  3. llvm-profdata merge -> out\pgo\predator.profdata;
#  4. FCP_PGO=use in the build folder of -Config (predator_code.dll with -fprofile-instr-use, same profile on the
#     player's first start when the release ships it, tools\dist.ps1).
# A training spec is "level|seconds|env" (env as for tools\boot\run.ps1, "k=v,k=v" separated by commas); the default
# set covers on-foot levels, boats, a vehicle and swimming. In a git worktree: -BuildCmd tools\boot\wt_build.cmd.
# -Off turns PGO off again in the build folder (FCP_PGO empty) and builds. -SkipTrain only merges the existing raw
# profiles; -NoBuild stops after the merge.
# The profile holds counters per function (names sub_<address>, structure hashes), no code; it is derived from
# runs of the game and stays under out\pgo unless the release ships it. A stale profile (after a codegen, hook or
# compiler change) is only slower, never wrong; clang does not report missing/mismatching functions (warnings
# off), so retrain after such changes.
param(
  [string]$Config = "release",
  [string]$BuildCmd = "build.cmd",
  [string]$Runs = "",
  [int]$Seconds = 55,
  [switch]$SkipTrain,
  [switch]$NoBuild,
  [switch]$Off
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bdir = Join-Path $root "out\build\$Config"
$pgo = Join-Path $root "out\pgo"
$raw = Join-Path $pgo "raw"
$toolchain = Join-Path $root "toolchain"
$tc = Select-String (Join-Path $bdir "CMakeCache.txt") -Pattern "^FCP_TOOLCHAIN_DIR:PATH=(.*)$" -ErrorAction SilentlyContinue
if ($tc) { $toolchain = $tc.Matches[0].Groups[1].Value }

# Default training set: level | seconds | extra environment. The input script walks and looks around; the
# vehicle runs place the player at a boat or a Humvee (FCP_TEST_PATCH) and drive with the input script.
$defaultRuns = @(
  "21_dunea|$Seconds|FCP_INPUT_SCRIPT=10-50:lup",
  "06_cliffs|$Seconds|FCP_INPUT_SCRIPT=10-50:lup",
  "00_training|$Seconds|FCP_INPUT_SCRIPT=10-50:lup",
  "26_shantynight|$Seconds|FCP_INPUT_SCRIPT=10-50:lup",
  "01_beach|$Seconds|FCP_INPUT_SCRIPT=10-50:lup",
  "03_river|40|FCP_TEST_PATCH=236.729,50.1575,28.6314|78.64900,424.710,19.000,FCP_INPUT_SCRIPT=8-9.5:x,12-38:rt+lup",
  "10_dead_marshes|40|FCP_TEST_PATCH=36.615,1005.85,21.8202|154.908,930.400,27.400,FCP_INPUT_SCRIPT=8-9.5:x,12-38:rt+lup",
  "13_black_forest|60|FCP_INPUT_SCRIPT=10-50:lup"
)

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

if (-not (Test-Path (Join-Path $bdir "CMakeCache.txt"))) { throw "build folder missing: $bdir (do a normal build first)" }
if ($Off) { SetPgo ""; Build; Write-Output "pgo: off"; exit 0 }

$specs = if ($Runs) { $Runs -split ";" } else { $defaultRuns }
if (-not $SkipTrain) {
  Write-Output "pgo: instrumented training build (out\build\pgo-train)"
  Push-Location $root
  try { cmd /c "tools\pgo_train_build.cmd > out\logs\build_pgo-train.log 2>&1" } finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw "training build failed (out\logs\build_pgo-train.log)" }
  New-Item -ItemType Directory -Force $raw | Out-Null
  Get-ChildItem $raw -Filter *.profraw -ErrorAction SilentlyContinue | Remove-Item
  # The training build has no shader pack of its own: use the one of the build folder (else the renderer
  # translates at run time and the training measures the translator).
  $pack = Join-Path $bdir "bin\shaders\predator.spirv.fcpack"
  foreach ($spec in $specs) {
    $parts = $spec -split "\|", 3
    $lv = $parts[0]; $sec = [int]$parts[1]
    # FCP_TEST_PATCH holds "|" itself: everything after the second "|" is the environment
    $extra = if ($parts.Count -gt 2) { $parts[2] -replace ',(?=FCP_|LLVM_)', ';' } else { "" }
    $envs = "FCP_FPS=120;FCP_START_LEVEL=$lv;FCP_INPUT_SCRIPT_CLOCK=level;LLVM_PROFILE_FILE=$raw\$lv-%p.profraw"
    if (Test-Path $pack) { $envs += ";FCP_SHADER_PACK=$pack" }
    if ($extra) { $envs += ";$extra" }
    Write-Output "pgo: training $lv ($sec s)"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run.ps1") -Config pgo-train `
      -Seconds $sec -Env $envs | Out-Null
    Copy-Item (Join-Path $root "out\build\pgo-train\bin\predator.log") (Join-Path $raw "$lv.log") -Force
  }
}
$files = @(Get-ChildItem $raw -Filter *.profraw | ForEach-Object FullName)
if ($files.Count -ne $specs.Count) { throw "pgo: $($files.Count) profiles for $($specs.Count) runs" }
& (Join-Path $toolchain "llvm\bin\llvm-profdata.exe") merge -o (Join-Path $pgo "predator.profdata") $files
if ($LASTEXITCODE -ne 0) { throw "llvm-profdata merge failed" }
if ($NoBuild) { Write-Output "pgo: merged $(Join-Path $pgo 'predator.profdata')"; exit 0 }
Write-Output "pgo: optimized build with $(Join-Path $pgo 'predator.profdata')"
SetPgo "use"; Build
Write-Output "pgo: done"
