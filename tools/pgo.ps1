# PGO of the recompiled code: instrumented training build, training runs, merge, optimized build.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pgo.ps1 [-Config release] [-BuildCmd build.cmd]
#       [-Runs <spec;spec>] [-Seconds 55] [-Out <file>] [-SkipTrain] [-NoBuild] [-Off] [-Auto]
# Steps:
#  1. tools\pgo_train_build.cmd: out\build\pgo-train, the game code linked into predator.exe and instrumented
#     (-DFCP_GAME_CODE_DLL=OFF -DFCP_PGO=generate; predator_code.dll has no CRT for the profile runtime);
#  2. one run per training spec (developer start of a level with an input script; written to out\pgo\raw);
#  3. llvm-profdata merge -> out\pgo\predator.profdata, copied to -Out (default config\predator\game_code.profdata,
#     the profile in the repository that every build uses by default, FCP_PGO=auto);
#  4. build of -Config with that profile (predator_code.dll with -fprofile-instr-use; the same profile on the
#     player's first start: tools\dist.ps1 ships it as compiler\pgo\predator.profdata). The build runs
#     tools\pgo_check.py and warns when the profile does not match the generated code.
# A training spec is "level|seconds|env" (env as for tools\boot\run.ps1, "k=v,k=v" separated by commas); the default
# set covers on-foot levels, boats, a vehicle and swimming. In a git worktree: -BuildCmd tools\boot\wt_build.cmd.
# -Off turns PGO off in the build folder (FCP_PGO=OFF) and builds; -Auto sets the default again (FCP_PGO=auto).
# -SkipTrain only merges the existing raw profiles; -NoBuild stops after the merge (and the copy to -Out).
# The profile holds counters per function (names sub_<address>, structure hashes), no code. Commit the new
# config\predator\game_code.profdata after training. A stale profile (after a codegen, title configuration, hook or
# compiler change) is only slower, never wrong; the build's tools\pgo_check.py reports it, then retrain.
param(
  [string]$Config = "release",
  [string]$BuildCmd = "build.cmd",
  [string]$Runs = "",
  [int]$Seconds = 55,
  [string]$Out = "",
  [switch]$SkipTrain,
  [switch]$NoBuild,
  [switch]$Off,
  [switch]$Auto
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bdir = Join-Path $root "out\build\$Config"
$pgo = Join-Path $root "out\pgo"
$raw = Join-Path $pgo "raw"
if (-not $Out) { $Out = Join-Path $root "config\predator\game_code.profdata" }
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

function SetCache([string]$name, [string]$type, [string]$value) {
  # Only change the entry in the CMake cache; the build then reconfigures by itself (build.ninja depends on the cache)
  # with the environment of the build script. A separate cmake call without this environment would reset the FCP_* paths.
  $cache = Join-Path $bdir "CMakeCache.txt"
  $lines = Get-Content $cache
  $found = $false
  $lines = $lines | ForEach-Object { if ($_ -match "^${name}:") { $found = $true; "${name}:${type}=$value" } else { $_ } }
  if (-not $found) { $lines += "${name}:${type}=$value" }
  Set-Content -Encoding ascii $cache $lines
}
function Build() {
  Push-Location $root
  try { cmd /c "$BuildCmd $Config" } finally { Pop-Location }
  if ($LASTEXITCODE -ne 0) { throw "build failed ($BuildCmd $Config)" }
}

if (-not (Test-Path (Join-Path $bdir "CMakeCache.txt"))) { throw "build folder missing: $bdir (do a normal build first)" }
if ($Off) { SetCache "FCP_PGO" "STRING" "OFF"; Build; Write-Output "pgo: off"; exit 0 }
if ($Auto) { SetCache "FCP_PGO" "STRING" "auto"; Build; Write-Output "pgo: auto"; exit 0 }

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
$merged = Join-Path $pgo "predator.profdata"
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
Copy-Item $merged $Out -Force
$sha = (Get-FileHash -Algorithm SHA256 $Out).Hash.ToLowerInvariant()
Write-Output "pgo: merged $Out ($((Get-Item $Out).Length) bytes, SHA-256 $sha)"
if ($NoBuild) { exit 0 }
Write-Output "pgo: optimized build with $Out"
SetCache "FCP_PGO" "STRING" "auto"; SetCache "FCP_PGO_PROFILE" "FILEPATH" ($Out -replace '\\', '/'); Build
Write-Output "pgo: done"
