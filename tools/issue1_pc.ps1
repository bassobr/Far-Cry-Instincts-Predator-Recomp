# Issue #1 picture check on the PC, the same views as on the Steam Deck (tools/deck/issue1.sh with
# VIEWS=issue1_views.py output; docs/phase1/issue-1-grafik.md 11). Runs a portable copy of a release folder with the
# reporter's settings and a 1280x800 window (the size gamescope gives the game on the Deck), under the PC measurement
# lock. Frame dumps land in <App>\logs, the log is copied to <Out>.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\issue1_pc.ps1 -App out\pc-i1 -Views out\deck\i1\views.env
#       -Out out\deck\i1\pc_v1 [-Env "FCP_RENDER_ASPECT=16:9;..."] [-Seconds 160]
param(
  [Parameter(Mandatory)][string]$App,
  [Parameter(Mandatory)][string]$Views,
  [Parameter(Mandatory)][string]$Out,
  [string]$Env = "",
  [int]$Seconds = 160,
  [int]$DumpEvery = 20,
  [string]$What = "issue1 pc views"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
. "$PSScriptRoot\measure_lock.ps1"
$App = (Resolve-Path $App).Path
$exe = Join-Path $App "predator.exe"
if (-not (Test-Path $exe)) { throw "missing: $exe" }
New-Item -ItemType Directory -Force $Out | Out-Null
$iso = Get-FcpGameIso
$toml = @"
[game]
image = '$iso'

[display]
mode = "windowed"
width = 1280
height = 800
fps = 45

[graphics]
backend = "vulkan"
msaa = "console"
smaa = true
scale = 1.5
resolution = "scale"
"@
[IO.File]::WriteAllText((Join-Path $App "predator.toml"), $toml)
if (-not (Test-Path (Join-Path $App "portable.txt"))) { [IO.File]::WriteAllText((Join-Path $App "portable.txt"), "") }
$vars = [ordered]@{
  "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"; "FCP_EXIT_AFTER" = "$Seconds"; "FCP_FPS_STATS" = "5"
  "FCP_START_LEVEL" = "03_river"; "FCP_INPUT_SCRIPT_CLOCK" = "level"
  "FCP_TEST_PATCH" = 'EntityClass="Merc"|EntityClass="Merx"'
  "FCP_TEST_PATCH2" = 'EntityClass="CreatureGenerator"|EntityClass="CreatureGeneratoX"'
  "FCP_DUMP_FRAMES" = "f_%d.png"; "FCP_DUMP_FRAME_EVERY" = "$DumpEvery"; "FCP_RENDER_STATS" = "450"
}
foreach ($l in Get-Content $Views) {
  $i = $l.IndexOf("=")
  if ($i -gt 0 -and $l.StartsWith("FCP_")) { $vars[$l.Substring(0, $i)] = $l.Substring($i + 1) }
}
foreach ($kv in $Env.Split(";")) {
  $i = $kv.IndexOf("=")
  if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
}
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }
$logs = Join-Path $App "logs"
if (Test-Path $logs) { Get-ChildItem $logs -Filter "f_*.png" | Remove-Item -Force }
$before = @(Get-Process predator -ErrorAction SilentlyContinue).Count
TakeLock $What
try {
  $start = Get-Date
  $p = Start-Process -FilePath $exe -WorkingDirectory $App -PassThru
  Write-Output "issue1_pc: PID $($p.Id) started $($start.ToString('HH:mm:ss')), foreign predator before: $before"
  $hard = $Seconds + 400  # first start: game code build and shader pack
  if (-not $p.WaitForExit($hard * 1000)) {
    Write-Output "issue1_pc: PID $($p.Id) still running after $hard s, ending it (this process only)"
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(10000) | Out-Null
  }
  $after = @(Get-Process predator -ErrorAction SilentlyContinue).Count
  Write-Output ("issue1_pc: done after {0:N1} s, exit code {1}, foreign predator after: {2}" -f ((Get-Date) - $start).TotalSeconds, $p.ExitCode, $after)
} finally { DropLock }
Copy-Item (Join-Path $App "predator.log") $Out -Force
Copy-Item (Join-Path $App "predator.toml") (Join-Path $Out "predator.toml.used") -Force
New-Item -ItemType Directory -Force (Join-Path $Out "logs") | Out-Null
Get-ChildItem $logs -Filter "f_*.png" | Move-Item -Destination (Join-Path $Out "logs") -Force
