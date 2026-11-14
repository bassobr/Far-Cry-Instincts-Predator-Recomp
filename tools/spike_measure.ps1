# Frame-time spikes in the warm state (second level visit: pipeline list, shader cache and driver cache filled).
# Runs predator.exe per level under the PC measurement lock (tools/measure_lock.ps1) with FCP_TRACE_FRAMES=1
# (every frame as CSV: duration, main thread CPU, time inside imports by category) and FCP_TRACE_HITCHES=<Threshold>
# (hitch lines with the main thread breakdown), Steam Deck approximation by default (affinity 0xFF, 60 frames/s).
# One user folder for all levels (warm caches); -Warmup first runs every level once for -WarmupSeconds.
# Evaluation: tools/frame_stats.py <frames.csv> [<predator.log>] (distribution, frame-to-frame jitter, causes).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\spike_measure.ps1 -Tag w1 -Levels 21_dunea,06_cliffs
#       [-Seconds 180] [-Warmup] [-WarmupSeconds 45] [-Fps 60] [-Affinity FF] [-Threshold 20] [-Extra "A=1;B=2"]
#       [-UserRoot out\spike\root] [-Bin <bin folder>] [-Profile 1] [-NoLock]
# Level names with a suffix pick an input script: 03_river:zodiac (ride the Zodiac, as bildraten-audit-2.md).
# Only the process started here is touched or ended (by PID). Logs: out\logs\spike\<Tag>_<level>.{log,csv}.
param(
  [string]$Tag = "run",
  [string[]]$Levels = @("21_dunea"),
  [int]$Seconds = 180,
  [switch]$Warmup,
  [int]$WarmupSeconds = 45,
  [string]$Fps = "60",
  [string]$Affinity = "FF",
  [int]$Threshold = 20,
  [string]$Extra = "",
  [string]$UserRoot = "",
  [string]$Bin = "",
  [int]$Profile = 0,
  [switch]$NoLock
)
$ErrorActionPreference = "Stop"
# powershell -File passes "a,b" as one string
$Levels = @($Levels | ForEach-Object { $_.Split(",") } | Where-Object { $_ })
. "$PSScriptRoot\shared_root.ps1"
. "$PSScriptRoot\measure_lock.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = if ($Bin) { if ([IO.Path]::IsPathRooted($Bin)) { $Bin } else { Join-Path $root $Bin } } else { Join-Path $root "out\build\release\bin" }
$exe = Join-Path $bin "predator.exe"
$outDir = Join-Path $root "out\logs\spike"
New-Item -ItemType Directory -Force $outDir | Out-Null
if (-not $UserRoot) { $UserRoot = Join-Path $root "out\spike\root" }
elseif (-not [IO.Path]::IsPathRooted($UserRoot)) { $UserRoot = Join-Path $root $UserRoot }
New-Item -ItemType Directory -Force $UserRoot | Out-Null
$nvCache = Join-Path $UserRoot "nvcache"
New-Item -ItemType Directory -Force $nvCache | Out-Null

function Foreign() {
  @(Get-Process predator -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or -not $_.Path.StartsWith($bin) }).Count
}

# Walk forward, turn now and then, fire a burst: keeps the player moving through the level for long runs.
function WalkScript([int]$secs) {
  $e = @()
  for ($t = 10; $t -lt $secs; $t += 20) {
    $e += "$t-$($t + 14):lup", "$($t + 14)-$($t + 15):rright:0.6", "$($t + 16)-$($t + 18):lup+rt"
  }
  $e -join ","
}

function LevelVars([string]$spec, [int]$secs) {
  $parts = $spec.Split(":")
  $level = $parts[0]
  $v = @{ "FCP_START_LEVEL" = $level }
  if ($parts.Count -gt 1 -and $parts[1] -eq "zodiac") {
    # Spawn on the Zodiac (03_river), get in, full throttle with alternating steering.
    $v["FCP_TEST_PATCH"] = "236.729,50.1575,28.6314|78.64900,424.710,19.000"
    $e = @("8-9.5:x")
    for ($t = 12; $t -lt $secs; $t += 8) { $e += "$t-$($t + 4):rt+lup", "$($t + 4)-$($t + 6):rt+lup=1+lleft=0.7", "$($t + 6)-$($t + 8):rt+lup=1+lright=0.5" }
    $v["FCP_INPUT_SCRIPT"] = $e -join ","
  } else {
    $v["FCP_INPUT_SCRIPT"] = WalkScript $secs
  }
  $v
}

function RunOne([string]$spec, [int]$secs, [string]$label) {
  $vars = @{
    "FCP_GAME_ISO" = (Get-FcpGameIso); "FCP_USER_ROOT" = $UserRoot
    "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"; "FCP_EXIT_AFTER" = "$secs"
    "REX_UI_WINDOW_WIDTH" = "640"; "REX_UI_WINDOW_HEIGHT" = "360"
    "FCP_CPU_STATS" = "10"; "FCP_FPS_STATS" = "10"; "FCP_FPS" = $Fps; "FCP_TRACE_HITCHES" = "$Threshold"
    "FCP_TRACE_FRAMES" = "1"; "FCP_INPUT_SCRIPT_CLOCK" = "level"
    "__GL_SHADER_DISK_CACHE_PATH" = $nvCache
  }
  if ($Profile -gt 0) { $vars["FCP_PROFILE"] = "$Profile"; $vars["FCP_PROFILE_WINDOW"] = "30" }
  $lv = LevelVars $spec $secs
  foreach ($k in $lv.Keys) { $vars[$k] = $lv[$k] }
  foreach ($kv in $Extra.Split(";")) {
    $i = $kv.IndexOf("=")
    if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
  }
  foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }
  $csv = Join-Path $UserRoot "logs\frames.csv"
  if (Test-Path $csv) { Remove-Item $csv }
  $before = Foreign
  $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
  if ($Affinity) {
    try { $p.ProcessorAffinity = [IntPtr][Convert]::ToInt64($Affinity, 16) } catch { Write-Output "spike: affinity not set: $_" }
  }
  $mid = -1
  if ($p.WaitForExit([Math]::Min(30000, ($secs + 90) * 1000))) { } else { $mid = Foreign }
  if (-not $p.WaitForExit(($secs + 90) * 1000)) {
    Write-Output "spike: PID $($p.Id) hangs, ending it (this process only)"
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(10000) | Out-Null
  }
  $after = Foreign
  foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
  $name = ($spec -replace ":", "_")
  $log = Join-Path $outDir ("{0}_{1}.log" -f $label, $name)
  Copy-Item (Join-Path $UserRoot "predator.log") $log -Force
  if (Test-Path $csv) { Copy-Item $csv (Join-Path $outDir ("{0}_{1}.csv" -f $label, $name)) -Force }
  Write-Output ("spike: {0} {1} done, foreign predator.exe before/during/after {2}/{3}/{4} -> {5}" -f $label, $spec, $before, $mid, $after, $log)
}

$started = [DateTime]::Now
# 60 s pause after this checkout's last release, also across script runs (stamp file next to the logs)
$stamp = Join-Path $outDir "last_release.txt"
if (Test-Path $stamp) {
  try { $script:LastRelease = [DateTime]::Parse((Get-Content $stamp -Raw).Trim(), [Globalization.CultureInfo]::InvariantCulture) } catch { }
}
# Priority window of another measurement job: neither queue nor start while the file exists.
$priority = Join-Path (Get-FcpSharedRoot) "out\pc-measure.priority"
while (-not $NoLock -and (Test-Path $priority)) { Start-Sleep -Seconds 30 }
if (-not $NoLock) { TakeLock "spike $Tag"; $started = [DateTime]::Now }
try {
  $jobs = @()
  if ($Warmup) { foreach ($l in $Levels) { $jobs += , @($l, $WarmupSeconds, "${Tag}warm") } }
  foreach ($l in $Levels) { $jobs += , @($l, $Seconds, $Tag) }
  foreach ($j in $jobs) {
    # Window of at most ~20 min: release and queue again when the next run would pass 19 min.
    if (-not $NoLock -and ([DateTime]::Now - $started).TotalSeconds + $j[1] + 40 -gt 19 * 60) {
      DropLock
      Write-Output "spike: lock released after $([int]([DateTime]::Now - $started).TotalMinutes) min, waiting again"
      TakeLock "spike $Tag"
      $started = [DateTime]::Now
    }
    RunOne $j[0] $j[1] $j[2]
  }
} finally {
  if (-not $NoLock) {
    DropLock
    Set-Content $stamp ([DateTime]::Now.ToString("o", [Globalization.CultureInfo]::InvariantCulture))
  }
}
