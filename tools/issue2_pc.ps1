# Issue #2 (lighting in 06_cliffs) and related checks on the PC: runs a portable copy of a release folder with the
# reporter's settings (Vulkan, 1366x768 window, resolution native, console MSAA, SMAA, 60 fps, vsync off) under the
# PC measurement lock. Frame dumps land in <App>\logs, the log and dumps are moved to <Out>.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\issue2_pc.ps1 -App out\app -Out out\issue2\run1
#       [-Env "FCP_X=1;FCP_Y=2"] [-Seconds 90] [-DumpEvery 0] [-Level 06_cliffs] [-Scale native|1.5|2]
#       [-UserRoot <copy of a user folder>]
param(
  [Parameter(Mandatory)][string]$App,
  [Parameter(Mandatory)][string]$Out,
  [string]$Env = "",
  [int]$Seconds = 90,
  [int]$DumpEvery = 0,
  [string]$Level = "06_cliffs",
  [string]$Scale = "native",
  [string]$Backend = "vulkan",
  [int]$Fps = 60,
  [string]$UserRoot = "",
  [string]$What = "issue2 pc",
  [switch]$NoLock,  # the caller holds the PC measurement lock (tools\issue2_batch.ps1)
  [switch]$NoEnemies
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
. "$PSScriptRoot\measure_lock.ps1"
$App = (Resolve-Path $App).Path
$exe = Join-Path $App "predator.exe"
if (-not (Test-Path $exe)) { throw "missing: $exe" }
New-Item -ItemType Directory -Force $Out | Out-Null
$iso = Get-FcpGameIso
$res = if ($Scale -eq "native") { "resolution = `"native`"" } else { "scale = $Scale`nresolution = `"scale`"" }
$toml = @"
[game]
image = '$iso'

[display]
mode = "windowed"
width = 1366
height = 768
fps = $Fps
vsync = false

[graphics]
backend = "$Backend"
msaa = "console"
smaa = true
$res
"@
$home_dir = if ($UserRoot) { (Resolve-Path $UserRoot).Path } else { $App }  # settings, logs and saves live here
[IO.File]::WriteAllText((Join-Path $home_dir "predator.toml"), $toml)
if (-not (Test-Path (Join-Path $App "portable.txt"))) { [IO.File]::WriteAllText((Join-Path $App "portable.txt"), "") }
$vars = [ordered]@{
  "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"; "FCP_EXIT_AFTER" = "$Seconds"; "FCP_FPS_STATS" = "5"
  "FCP_INPUT_SCRIPT_CLOCK" = "level"; "FCP_RENDER_STATS" = "600"
}
if ($Level) { $vars["FCP_START_LEVEL"] = $Level }
if ($NoEnemies) {
  # test switch (changes the game): mercenaries and creature generators not spawned (issue-1-grafik.md 10.3)
  $vars["FCP_TEST_PATCH"] = 'EntityClass="Merc"|EntityClass="Merx"'
  $vars["FCP_TEST_PATCH2"] = 'EntityClass="CreatureGenerator"|EntityClass="CreatureGeneratoX"'
}
if ($UserRoot) { $vars["FCP_USER_ROOT"] = (Resolve-Path $UserRoot).Path }
if ($DumpEvery -gt 0) { $vars["FCP_DUMP_FRAMES"] = "f_%d.png"; $vars["FCP_DUMP_FRAME_EVERY"] = "$DumpEvery" }
foreach ($kv in $Env.Split(";")) {
  $i = $kv.IndexOf("=")
  if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
}
# A batch runs several of these in one process: no FCP_* variable of an earlier run may carry over.
foreach ($e in @(Get-ChildItem Env: | Where-Object { $_.Name -like "FCP_*" -and $_.Name -notin @("FCP_SHARED_ROOT", "FCP_GAME_ISO") })) {
  [Environment]::SetEnvironmentVariable($e.Name, $null, "Process")
}
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }
$logs = Join-Path $home_dir "logs"
if (Test-Path $logs) { Get-ChildItem $logs -Filter "f_*.png" | Remove-Item -Force }
$before = @(Get-Process predator -ErrorAction SilentlyContinue).Count
if (-not $NoLock) { TakeLock $What }
try {
  $start = Get-Date
  $p = Start-Process -FilePath $exe -WorkingDirectory $App -PassThru
  Write-Output "issue2_pc: PID $($p.Id) started $($start.ToString('HH:mm:ss')), foreign predator before: $before"
  $hard = $Seconds + 400  # first start: game code build and shader pack
  # Dedicated/shared GPU memory of this process (Windows GPU counters), every 5 s, into vram.csv
  $vram = @("t_s,dedicated_mb,shared_mb")
  while (-not $p.WaitForExit(5000)) {
    if (((Get-Date) - $start).TotalSeconds -gt $hard) { break }
    try {
      $cs = (Get-Counter "\GPU Process Memory(pid_$($p.Id)_*)\Dedicated Usage", "\GPU Process Memory(pid_$($p.Id)_*)\Shared Usage" -ErrorAction Stop).CounterSamples
      $ded = ($cs | Where-Object { $_.Path -like "*dedicated usage" } | Measure-Object CookedValue -Sum).Sum / 1MB
      $sha = ($cs | Where-Object { $_.Path -like "*shared usage" } | Measure-Object CookedValue -Sum).Sum / 1MB
      $vram += [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:F0},{1:F1},{2:F1}", ((Get-Date) - $start).TotalSeconds, $ded, $sha)
    } catch { }
  }
  [IO.File]::WriteAllLines((Join-Path $Out "vram.csv"), $vram)
  if (-not $p.HasExited) {
    Write-Output "issue2_pc: PID $($p.Id) still running after $hard s, ending it (this process only)"
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(10000) | Out-Null
  }
  $after = @(Get-Process predator -ErrorAction SilentlyContinue).Count
  Write-Output ("issue2_pc: done after {0:N1} s, exit code {1}, foreign predator after: {2}" -f ((Get-Date) - $start).TotalSeconds, $p.ExitCode, $after)
} finally { if (-not $NoLock) { DropLock } }
Get-ChildItem $home_dir -Filter "predator*.log" | Copy-Item -Destination $Out -Force
Copy-Item (Join-Path $home_dir "predator.toml") (Join-Path $Out "predator.toml.used") -Force
Get-ChildItem $logs -Filter "*.csv" -ErrorAction SilentlyContinue | Move-Item -Destination $Out -Force
$dumps = @(Get-ChildItem $logs -Filter "f_*.png" -ErrorAction SilentlyContinue)
if ($dumps.Count) {
  New-Item -ItemType Directory -Force (Join-Path $Out "dumps") | Out-Null
  $dumps | Move-Item -Destination (Join-Path $Out "dumps") -Force
}
