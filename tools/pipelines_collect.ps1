# Collects the pipeline records for the shipped pipeline list (tools\pipelines_build.ps1 builds the list from them).
# Every run has FCP_PIPELINE_COLLECT=<folder>: the renderer writes each pipeline record a draw uses into
# <folder>\<group>.bin ("menu" before the first level, then the level name), whether the pipeline was new or not.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\pipelines_collect.ps1 [-Set all|menu,levels,vehicles,transitions]
#       [-Levels 00_training,21_dunea] [-Seconds 75] [-Out out\pipelines\collect\<name>] [-Config release] [-Env "A=1"]
#       [-NoLock]
# Sets:
#   menu         logos, intro video, title, main menu, new game (input script as tools\stability.ps1), loading
#                screen and the start of 00_training
#   levels       the 19 story levels (developer start), walking, looking around and firing (key script of
#                tools\level_sweep.ps1)
#   vehicles     the boat and Humvee runs of tools\pgo.ps1 (03_river, 10_dead_marshes)
#   transitions  both campaigns as restart chains (tools\level_transitions.ps1: every transition, both credits)
# Each run takes the PC measurement lock (tools\measure_lock.ps1) for at most -Batch runs, then releases it for
# 60 s. Only the process started here is ended (by PID, tools\boot\run.ps1). Output is gitignored (out\).
param(
  [string]$Set = "all",
  [string[]]$Levels = @(),
  [int]$Seconds = 75,
  [string]$Out = "",
  [string]$Config = "release",
  [string[]]$Env = @(),
  [int]$Batch = 10,
  [switch]$NoLock
)
$ErrorActionPreference = "Stop"
$sets = @($Set.Split(",") | ForEach-Object { $_.Trim() })
if ($sets -contains "all") { $sets = @("menu", "levels", "vehicles", "transitions") }
. "$PSScriptRoot\shared_root.ps1"
. "$PSScriptRoot\measure_lock.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Out) { $Out = Join-Path $root ("out\pipelines\collect\" + (Get-Date -Format "yyyyMMdd_HHmmss")) }
elseif (-not [IO.Path]::IsPathRooted($Out)) { $Out = Join-Path $root $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
$user = Join-Path $Out "user"
if ($Levels.Count -eq 1 -and $Levels[0].Contains(",")) { $Levels = $Levels[0].Split(",") }
if ($Levels.Count -eq 0) {
  $Levels = @("00_training", "01_beach", "02_native", "03_river", "04_rain_forest", "05_mines", "06_cliffs",
    "07_sub-aquatic_facility", "10_dead_marshes", "11_wasteland", "13_black_forest",
    "21_dunea", "22_duneb", "23_dunec", "24_shantyday", "26_shantynight", "28_jungle", "29_forest", "30_mountain")
}
$keyScript = "3-12:w,13-15:mx=600,16-18:mouse1,20-30:w,31:my=-120,33-38:mouse1+w,40-50:w+d,52-54:mx=-900,55-65:w,66-70:mouse1,72:my=120,73-85:w+a"
$common = @("FCP_PIPELINE_COLLECT=$Out", "FCP_USER_ROOT=$user", "FCP_FPS=60", "FCP_RENDER_PIPELINE_SHIPPED=0") + $Env

# job list: name, kind, data
$jobs = New-Object System.Collections.Generic.List[object]
if ($sets -contains "menu") {
  $jobs.Add(@{ Name = "menu"; Seconds = 100; Env = @("FCP_INPUT_SCRIPT=34:start,38:start,42:a,46:a,50:a,54:a,58:a,62:a") })
}
if ($sets -contains "levels") {
  foreach ($l in $Levels) {
    $jobs.Add(@{ Name = $l; Seconds = $Seconds; Env = @("FCP_START_LEVEL=$l", "FCP_INPUT_SCRIPT_CLOCK=level", "FCP_KEY_SCRIPT=$keyScript") })
  }
}
if ($sets -contains "vehicles") {
  $jobs.Add(@{ Name = "03_river boat"; Seconds = 45; Env = @("FCP_START_LEVEL=03_river", "FCP_INPUT_SCRIPT_CLOCK=level",
        "FCP_TEST_PATCH=236.729,50.1575,28.6314|78.64900,424.710,19.000", "FCP_INPUT_SCRIPT=8-9.5:x,12-38:rt+lup") })
  $jobs.Add(@{ Name = "10_dead_marshes humvee"; Seconds = 45; Env = @("FCP_START_LEVEL=10_dead_marshes", "FCP_INPUT_SCRIPT_CLOCK=level",
        "FCP_TEST_PATCH=36.615,1005.85,21.8202|154.908,930.400,27.400", "FCP_INPUT_SCRIPT=8-9.5:x,12-38:rt+lup") })
}
if ($sets -contains "transitions") {
  $jobs.Add(@{ Name = "transitions instincts"; Transitions = "instincts" })
  $jobs.Add(@{ Name = "transitions evolution"; Transitions = "evolution" })
}

$held = 0
$lastDrop = [DateTime]::MinValue
function Take([string]$what) {
  if ($NoLock) { return }
  $wait = 60 - ([DateTime]::Now - $lastDrop).TotalSeconds
  if ($wait -gt 0) { Start-Sleep -Seconds ([int][Math]::Ceiling($wait)) }
  TakeLock $what
}
function Drop() {
  if ($NoLock) { return }
  DropLock
  $script:lastDrop = [DateTime]::Now
}

$locked = $false
$since = [DateTime]::Now
foreach ($j in $jobs) {
  # at most -Batch runs or ~10 minutes per window before a transition chain (it runs for several minutes)
  $minutes = ([DateTime]::Now - $since).TotalMinutes
  if ($locked -and ($held -ge $Batch -or ($j.Transitions -and $minutes -gt 10))) { Drop; $locked = $false; $held = 0 }
  if (-not $locked) { Take "pipeline collection ($($j.Name))"; $locked = $true; $since = [DateTime]::Now }
  Write-Output "pipelines_collect: $($j.Name) ..."
  if ($j.Transitions) {
    # level_transitions.ps1 reads the logs from the bin folder: no own user folder here
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\level_transitions.ps1") -Config $Config `
      -Campaign $j.Transitions -Env (($common | Where-Object { $_ -notlike "FCP_USER_ROOT=*" }) -join ";") | Select-Object -Last 3
    $held++
  } else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run.ps1") -Config $Config `
      -Seconds $j.Seconds -Env (($common + $j.Env) -join ";") | Select-Object -Last 1
    $held++
  }
}
if ($locked) { Drop }
$files = Get-ChildItem $Out -Filter *.bin -File
foreach ($f in $files | Sort-Object Name) { Write-Output ("  {0,-30} {1,6} records" -f $f.BaseName, [int](($f.Length - 8) / 104)) }
Write-Output "pipelines_collect: done, $Out (tools\pipelines_build.ps1 -In $Out)"
